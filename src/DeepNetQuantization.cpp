/*
    (C) Copyright 2019 CEA LIST. All Rights Reserved.
    Contributor(s): Olivier BICHLER (olivier.bichler@cea.fr)

    This software is governed by the CeCILL-C license under French law and
    abiding by the rules of distribution of free software.  You can  use,
    modify and/ or redistribute the software under the terms of the CeCILL-C
    license as circulated by CEA, CNRS and INRIA at the following URL
    "http://www.cecill.info".

    As a counterpart to the access to the source code and  rights to copy,
    modify and redistribute granted by the license, users are provided only
    with a limited warranty  and the software's author,  the holder of the
    economic rights,  and the successive licensors  have only  limited
    liability.

    The fact that you are presently reading this means that you have had
    knowledge of the CeCILL-C license and that you accept its terms.
*/

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "DeepNet.hpp"
#include "DeepNetQuantization.hpp"
#include "Histogram.hpp"
#include "RangeStats.hpp"
#include "ScalingMode.hpp"
#include "StimuliProvider.hpp"
#include "ScalingMode.hpp"
#include "Activation/LinearActivation.hpp"
#include "Activation/LinearActivation_Frame.hpp"
#include "Activation/LogisticActivation.hpp"
#include "Activation/RectifierActivation.hpp"
#include "Activation/RectifierActivation_Frame.hpp"
#include "Activation/SaturationActivation.hpp"
#include "Cell/Cell.hpp"
#include "Cell/Cell_Frame_Top.hpp"
#include "Cell/ElemWiseCell.hpp"
#include "Cell/PoolCell.hpp"
#include "Cell/ScalingCell.hpp"
#include "Export/DeepNetExport.hpp"


N2D2::DeepNetQuantization::DeepNetQuantization(DeepNet& deepNet): mDeepNet(deepNet) {
}

void N2D2::DeepNetQuantization::clipWeights(std::size_t nbBits, ClippingMode wtClippingMode) {
    if(wtClippingMode == ClippingMode::NONE) {
        return;
    }

    const std::size_t nbBins = getNbBinsForClippingMode(nbBits, wtClippingMode);

    const std::vector<std::vector<std::string>>& layers = mDeepNet.getLayers();
    for (auto it = layers.begin() + 1; it != layers.end(); ++it) {
        for (auto itCell = it->begin(); itCell != it->end(); ++itCell) {
            const auto& cell = mDeepNet.getCell(*itCell);

            const auto range = cell->getFreeParametersRange(false);
            const Float_T maxWeight = Utils::max_abs(range.first, range.second);

            if(maxWeight == 0) {
                continue;
            }

            Histogram hist(-maxWeight, maxWeight, nbBins);
            cell->processFreeParameters([&](Float_T wt) { 
                hist(wt);
                return wt; 
            }, Cell::Multiplicative);

            Float_T threshold;
            switch(wtClippingMode) {
                case ClippingMode::KL_DIVERGENCE:
                    threshold = hist.calibrateKLDivergence(nbBits);
                    break;
                case ClippingMode::MSE:
                    threshold = hist.calibrateMSE(nbBits);
                    break;
                default:
                    throw std::runtime_error("Unsupported clipping mode.");
            }

            cell->processFreeParameters([&](Float_T wt) { 
                return Utils::clamp(wt, -threshold, threshold); 
            }, Cell::Multiplicative);
        }
    }
}

void N2D2::DeepNetQuantization::normalizeFreeParameters(Float_T normFactor) {
    std::unordered_map<std::string, Float_T> bScalingForCell;

    // Get a copy, the loop may modify the graph
    const std::vector<std::vector<std::string>> layers = mDeepNet.getLayers();
    for(auto itLayer = layers.begin() + 1; itLayer != layers.end(); ++itLayer) {
        for(auto itCell = itLayer->begin(); itCell != itLayer->end(); ++itCell) {
            std::shared_ptr<Cell> cell = mDeepNet.getCell(*itCell);
            if(!cell) {
                throw std::runtime_error("Invalid cell.");
            }


            Float_T bScalingCell = getMaxParentsScaling(cell, bScalingForCell);
            rescaleParentsToScaling(cell, bScalingForCell, bScalingCell);


            const auto wMinMax = cell->getFreeParametersRange(false);
            const Float_T wScalingCell = Utils::max_abs(wMinMax.first, wMinMax.second)/normFactor;
            if(wScalingCell != 0.0) {
                cell->processFreeParameters([&](Float_T w) { return w/wScalingCell; }, Cell::Multiplicative);
                bScalingCell *= wScalingCell;
            }

            cell->processFreeParameters([&](Float_T b) { return b/bScalingCell; }, Cell::Additive);
            bScalingForCell[cell->getName()] = bScalingCell;
        }
    }


    fuseScalingCells();
}

void N2D2::DeepNetQuantization::normalizeFreeParametersPerOutputChannel(Float_T normFactor) {
    std::unordered_map<std::string, Float_T> bScalingForCell;

    // Get a copy, the loop may modify the graph
    const std::vector<std::vector<std::string>> layers = mDeepNet.getLayers();
    for (auto itLayer = layers.begin() + 1; itLayer != layers.end(); ++itLayer) {
        for(auto itCell = itLayer->begin(); itCell != itLayer->end(); ++itCell) {
            std::shared_ptr<Cell> cell = mDeepNet.getCell(*itCell);
            if(!cell) {
                throw std::runtime_error("Invalid cell.");
            }


            Float_T bScalingCell = getMaxParentsScaling(cell, bScalingForCell);
            rescaleParentsToScaling(cell, bScalingForCell, bScalingCell);


            if(bScalingCell != 1.0) {
                cell->processFreeParameters([&](Float_T b) { return b/bScalingCell; }, Cell::Additive);
            }

            const auto wMinMax = cell->getFreeParametersRange(false);
            const Float_T wScalingCell = Utils::max_abs(wMinMax.first, wMinMax.second);
            if(wScalingCell == 0) {
                bScalingForCell[cell->getName()] = bScalingCell;
            }
            else {
                std::vector<Float_T> scalingPerOutput(cell->getNbOutputs());
                for(std::size_t output = 0; output < cell->getNbOutputs(); output++) {
                    const auto woMinMax = cell->getFreeParametersRangePerOutput(output, false);
                    const Float_T wScalingCellOutput = std::max(
                                                         std::min(wScalingCell, 0.1f), 
                                                         Utils::max_abs(woMinMax.first, woMinMax.second)
                                                       )/normFactor;

                    cell->processFreeParametersPerOutput([&](Float_T d) { return d/wScalingCellOutput; }, 
                                                         output);
                    scalingPerOutput[output] = wScalingCellOutput/wScalingCell;
                }


                auto scalingCell = Registrar<ScalingCell>::create<Float_T>(getCellModelType(*cell))
                                        (mDeepNet, cell->getName() + "_rescale_params", 
                                         cell->getNbOutputs(), 
                                         Scaling::floatingPointScaling(
                                             std::move(scalingPerOutput))
                                        );
                errorIfCellExist(scalingCell->getName());
                mDeepNet.addCellAfter(scalingCell, cell);


                bScalingForCell[scalingCell->getName()] = bScalingCell*wScalingCell;
            }
        }
    }
    

    fuseScalingCells();
}

N2D2::Float_T N2D2::DeepNetQuantization::getMaxParentsScaling(const std::shared_ptr<Cell>& cell, 
                                const std::unordered_map<std::string, Float_T>& scalingForCell) const 
{
    Float_T maxParentsScaling = 0.0;
    for(const auto& parentCell: cell->getParentsCells()) {
        const Float_T parentScaling = parentCell?scalingForCell.at(parentCell->getName()):1.0;
        maxParentsScaling = std::max(maxParentsScaling, parentScaling);

        assert(parentScaling > 0.0);
    }

    return maxParentsScaling;
}

void N2D2::DeepNetQuantization::rescaleParentsToScaling(const std::shared_ptr<Cell>& cell, 
                                        const std::unordered_map<std::string, Float_T>& scalingForCell,
                                        Float_T scaling)
{
    // Get a copy, the loop modify the graph
    auto parentsCells = cell->getParentsCells();

    for(const auto& parentCell: parentsCells) {
        const Float_T parentScaling = parentCell?scalingForCell.at(parentCell->getName()):1.0;
        if(parentScaling == scaling) {
            continue;
        }
        
        assert(parentScaling < scaling);
        auto scalingCell = Registrar<ScalingCell>::create<Float_T>(getCellModelType(*parentCell))
                                (mDeepNet, parentCell->getName() + "_rescale_branch", 
                                 parentCell->getNbOutputs(), 
                                 Scaling::floatingPointScaling(
                                     std::vector<Float_T>(parentCell->getNbOutputs(), 
                                                          parentScaling/scaling))
                                );

        errorIfCellExist(scalingCell->getName());
        mDeepNet.addCellBetween(scalingCell, parentCell, cell);
    }
}

void N2D2::DeepNetQuantization::rescaleAdditiveParameters(Float_T rescaleFactor) {
    const std::vector<std::vector<std::string>>& layers = mDeepNet.getLayers();

    for (auto it = layers.begin() + 1; it != layers.end(); ++it) {
        for (auto itCell = it->begin(); itCell != it->end(); ++itCell) {
            std::shared_ptr<Cell> cell = mDeepNet.getCell(*itCell);
            cell->processFreeParameters([&](Float_T v) { return v/rescaleFactor; }, 
                                        Cell::Additive);
        }
    }
}

void N2D2::DeepNetQuantization::reportOutputsRange(std::unordered_map<std::string, RangeStats>& outputsRange) const {
    const std::vector<std::vector<std::string>>& layers = mDeepNet.getLayers();
    std::map<std::string, std::shared_ptr<Cell>>& cells = mDeepNet.getCells();

    if (outputsRange.empty()) {
        // Populate outputsRange first to avoid thread issues
        for (auto itLayer = layers.begin(); itLayer != layers.end(); ++itLayer) {
            for(auto itCell = itLayer->begin(); itCell != itLayer->end(); ++itCell) {
                outputsRange.insert(std::make_pair(*itCell, RangeStats()));
            }
        }
    }

#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < (int)layers.size(); ++i) {
        for(auto itCell = layers[i].begin(); itCell != layers[i].end(); ++itCell) {
            std::shared_ptr<Cell_Frame_Top> cellFrame;

            if (cells.find(*itCell) != cells.end()) {
                cellFrame = std::dynamic_pointer_cast<Cell_Frame_Top>(cells.at(*itCell));
                cellFrame->getOutputs().synchronizeDToH();
            }

            const Tensor<Float_T>& outputs = (cellFrame)
                ? tensor_cast<Float_T>(cellFrame->getOutputs())
                : mDeepNet.getStimuliProvider()->getData();

            RangeStats& rangeStats = outputsRange.at(*itCell);
            assert(outputs.size() == outputs.dimB()*outputs.dimZ()*outputs.dimY()*outputs.dimX());
            
            for(std::size_t batch = 0; batch < outputs.dimB(); batch++) {
                if(mDeepNet.getStimuliProvider()->getBatch().at(batch) == -1) {
                    continue;
                }
                
                for(Float_T val: outputs[batch]) {
                    rangeStats(val);
                }
            }
        }
    }
}

void N2D2::DeepNetQuantization::reportOutputsHistogram(
                        std::unordered_map<std::string, Histogram>& outputsHistogram,
                        const std::unordered_map<std::string, RangeStats>& outputsRange,
                        std::size_t nbBits, ClippingMode actClippingMode) const
{
    if(actClippingMode == ClippingMode::NONE) {
        return;
    }

    const std::size_t nbBins = getNbBinsForClippingMode(nbBits, actClippingMode);
    const std::vector<std::vector<std::string>>& layers = mDeepNet.getLayers();
    std::map<std::string, std::shared_ptr<Cell>>& cells = mDeepNet.getCells();

    if (outputsHistogram.empty()) {
        // Populate outputsHistogram first to avoid thread issues
        for (auto itLayer = layers.begin(); itLayer != layers.end(); ++itLayer) {
            for(auto itCell = itLayer->begin(); itCell != itLayer->end(); ++itCell) {
                const auto range = outputsRange.at(*itCell);
                const bool isCellOutputUnsigned = (itLayer == layers.begin())?
                                            DeepNetExport::mEnvDataUnsigned:
                                            DeepNetExport::isCellOutputUnsigned(*cells.at(*itCell));
                
                double val = Utils::max_abs(range.minVal(), range.maxVal());
                // Take 0.1 as minimum value as we don't want a range of [0;0]
                val = std::max(val, 0.1);

                const double min = isCellOutputUnsigned?0:-val;
                const double max = val;
                outputsHistogram.insert(std::make_pair(*itCell, Histogram(min, max, nbBins)));
            }
        }
    }

    #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < (int)layers.size(); ++i) {
        for(auto itCell = layers[i].begin(); itCell != layers[i].end(); ++itCell) {
            std::shared_ptr<Cell_Frame_Top> cellFrame;

            if (cells.find(*itCell) != cells.end()) {
                cellFrame = std::dynamic_pointer_cast<Cell_Frame_Top>(cells.at(*itCell));
                cellFrame->getOutputs().synchronizeDToH();
            }

            const Tensor<Float_T>& outputs = (cellFrame)
                ? tensor_cast<Float_T>(cellFrame->getOutputs())
                : mDeepNet.getStimuliProvider()->getData();


            Histogram& hist = outputsHistogram.at(*itCell);
            assert(outputs.size() == outputs.dimB()*outputs.dimZ()*outputs.dimY()*outputs.dimX());

            const auto range = outputsRange.at(*itCell);
            const bool enlargeSymetric = hist.getMinVal() < 0.0;
            hist.enlarge(Utils::max_abs(range.minVal(), range.maxVal()), enlargeSymetric);

            for(std::size_t batch = 0; batch < outputs.dimB(); batch++) {
                if(mDeepNet.getStimuliProvider()->getBatch().at(batch) == -1) {
                    continue;
                }

                for(Float_T val: outputs[batch]) {
                    hist(val);
                }
            }
        }
    }
}

void N2D2::DeepNetQuantization::normalizeOutputsRange(
                            const std::unordered_map<std::string, Histogram>& outputsHistogram,
                            const std::unordered_map<std::string, RangeStats>& outputsRange,
                            std::size_t nbBits,
                            ClippingMode actClippingMode)
{
    std::unordered_map<std::string, Float_T> scalingFactorForCell;

    const std::vector<std::vector<std::string>> layers = mDeepNet.getLayers();
    for (auto itLayer = layers.begin() + 1; itLayer != layers.end(); ++itLayer) {
        for (auto itCell = itLayer->begin(); itCell != itLayer->end(); ++itCell) {
            std::shared_ptr<Cell> cell = mDeepNet.getCell(*itCell);
            std::shared_ptr<Cell_Frame_Top> cellFrame = std::dynamic_pointer_cast<Cell_Frame_Top>(cell);
            if(!cell || !cellFrame) {
                throw std::runtime_error("Invalid cell.");
            }


            const Float_T prevScalingFactor = getMaxParentsScaling(cell, scalingFactorForCell);
            rescaleParentsToScaling(cell, scalingFactorForCell, prevScalingFactor);



            Float_T scalingFactor;
            const std::shared_ptr<Activation>& activation = cellFrame->getActivation();
            if(activation) {
                const bool clip =  cell->getNbOutputs() > 2 && 
                                   (activation->getType() == RectifierActivation::Type || 
                                    activation->getType() == LinearActivation::Type || 
                                    activation->getType() == SaturationActivation::Type);
                

                /**
                 * When clipping with MSE or KL-Divergence and the next cell is a max pooling cell,
                 * use the histogram of the max pooling to calculate the clipping threshold.
                 */
                auto childrenCells = cell->getChildrenCells();
                const bool isNextCellMaxPool = childrenCells.size() == 1 && 
                                               childrenCells[0]->getType() == PoolCell::Type && 
                                               dynamic_cast<const PoolCell&>(*childrenCells[0]).getPooling() == PoolCell::Max;


                const std::string cellStatsName = clip && isNextCellMaxPool?childrenCells[0]->getName():
                                                                            *itCell;
                scalingFactor = getCellThreshold(cellStatsName, 
                                                 outputsHistogram, outputsRange, 
                                                 nbBits, clip?actClippingMode:ClippingMode::NONE);
            }
            else if(cell->getType() == ElemWiseCell::Type) {
                scalingFactor = getCellThreshold(*itCell,
                                                 outputsHistogram, outputsRange, 
                                                 nbBits, ClippingMode::NONE);
            }
            else if(cell->getType() == PoolCell::Type || cell->getType() == ScalingCell::Type) {
                std::cout << "No scaling for cell " << cell->getName() << "." << std::endl;
                scalingFactorForCell[cell->getName()] = prevScalingFactor;
                continue;
            }
            else {
                throw std::runtime_error("Quantization of cell '" + cell->getName() + "' of type '" + 
                                         cell->getType() + "' is not supported yet.");
            }


            scalingFactorForCell[cell->getName()] = scalingFactor;


            


            cell->processFreeParameters([&](Float_T d) { return d/prevScalingFactor; },
                                        Cell::Additive);

            auto scalingCell = Registrar<ScalingCell>::create<Float_T>(getCellModelType(*cell))
                                    (mDeepNet, 
                                     cell->getName() + "_rescale_act", 
                                     cell->getNbOutputs(), 
                                     Scaling::floatingPointScaling(
                                         std::vector<Float_T>(cell->getNbOutputs(), 
                                                              1/(scalingFactor/prevScalingFactor)))
                                    );
            errorIfCellExist(scalingCell->getName());

            mDeepNet.addCellAfter(scalingCell, cell);
            scalingFactorForCell[scalingCell->getName()] = scalingFactorForCell[cell->getName()];

            std::cout << std::setprecision(4) 
                      << cell->getName() << ": " << "scalingFactor = " << scalingFactor << "   " 
                      << std::endl;
        }
    }


    fuseScalingCells();
}

void N2D2::DeepNetQuantization::fuseScalingCells() {
    // Get a copy, the loop may modify the graph
    const std::vector<std::vector<std::string>> layers = mDeepNet.getLayers();

    for (auto itLayer = layers.begin() + 1; itLayer != layers.end(); ++itLayer) {
        for (auto itCell = itLayer->begin(); itCell != itLayer->end(); ++itCell) {
            std::shared_ptr<Cell> cell = mDeepNet.getCell(*itCell);
            if(!cell) {
                throw std::runtime_error("Invalid cell.");
            }

            if(cell->getType() != ScalingCell::Type) {
                continue;
            }

            auto parentsCells = cell->getParentsCells();
            if(parentsCells.size() != 1 || !parentsCells.front() || 
               parentsCells.front()->getChildrenCells().size() != 1) 
            {
                continue;
            }


            auto scalingCell = std::dynamic_pointer_cast<ScalingCell>(cell);

            auto parentCell = parentsCells.front();
            auto parentCellFrame = std::dynamic_pointer_cast<Cell_Frame_Top>(parentCell);
            if(!parentCellFrame) {
                throw std::runtime_error("Invalid cell.");
            }


            std::shared_ptr<Activation> parentCellActivation = parentCellFrame->getActivation();
            if(parentCellActivation)  {
                fuseScalingCellWithParentActivation(scalingCell, *parentCellActivation);
            }
            else if(parentCell->getType() == ScalingCell::Type) {
                auto parentScalingCell = std::dynamic_pointer_cast<ScalingCell>(parentCell);
                fuseScalingCellWithParentScalingCell(scalingCell, parentScalingCell);
            }
            else if(parentCell->getType() == ElemWiseCell::Type) {
                auto parentElemWiseCell = std::dynamic_pointer_cast<ElemWiseCell>(parentCell);
                moveScalingCellAboveParentElemWiseCell(scalingCell, parentElemWiseCell);

                // The ScalingCell has been potentially moved as parent of the ElemeWiseCell.
                // Recurse to try to merge this ScalingCell with its new parents.
                fuseScalingCells();
                return;
            }
        }
    }
}

void N2D2::DeepNetQuantization::fuseScalingCellWithParentActivation(
                                                    const std::shared_ptr<ScalingCell>& scalingCell, 
                                                    Activation& parentCellActivation)
{
    const ScalingMode parentScalingMode = parentCellActivation.getActivationScaling().getMode();
    if(parentScalingMode == ScalingMode::NONE) {
        parentCellActivation.setActivationScaling(
            std::move(scalingCell->getScaling())
        );

        mDeepNet.removeCell(scalingCell);
    }
    else if(parentScalingMode == ScalingMode::FLOAT_MULT) {
        const std::vector<Float_T>& scalingPerOutput = scalingCell->getScaling()
                                                                   .getFloatingPointScaling()
                                                                   .getScalingPerOutput();
        std::vector<Float_T> parentScalingPerOutput = parentCellActivation.getActivationScaling()
                                                                          .getFloatingPointScaling()
                                                                          .getScalingPerOutput();
        for(std::size_t o = 0; o < parentScalingPerOutput.size(); o++) {
            parentScalingPerOutput[o] *= scalingPerOutput[o];
        }

        parentCellActivation.setActivationScaling(
            Scaling::floatingPointScaling(std::move(parentScalingPerOutput))
        );

        mDeepNet.removeCell(scalingCell);
    }
}

void N2D2::DeepNetQuantization::fuseScalingCellWithParentScalingCell(
                                        const std::shared_ptr<ScalingCell>& scalingCell, 
                                        const std::shared_ptr<ScalingCell>& parentScalingCell)
{
    assert(scalingCell->getNbOutputs() == parentScalingCell->getNbOutputs());

    const std::vector<Float_T>& scalingPerOutput = scalingCell->getScaling()
                                                               .getFloatingPointScaling()
                                                               .getScalingPerOutput();
    std::vector<Float_T> parentScalingPerOutput = parentScalingCell->getScaling()
                                                                    .getFloatingPointScaling()
                                                                    .getScalingPerOutput();
    for(std::size_t o = 0; o < parentScalingPerOutput.size(); o++) {
        parentScalingPerOutput[o] *= scalingPerOutput[o];
    }

    parentScalingCell->setScaling(Scaling::floatingPointScaling(std::move(parentScalingPerOutput)));

    mDeepNet.removeCell(scalingCell);
}

void N2D2::DeepNetQuantization::moveScalingCellAboveParentElemWiseCell(
                                        const std::shared_ptr<ScalingCell>& scalingCell, 
                                        const std::shared_ptr<ElemWiseCell>& parentElemWiseCell)
{
    const auto& weights = parentElemWiseCell->getWeights();
    const auto& shifts = parentElemWiseCell->getShifts();

    if(parentElemWiseCell->getOperation() == ElemWiseCell::Sum && 
        std::all_of(weights.begin(), weights.end(), [](Float_T v) { return v == 1.0; }) && 
        std::all_of(shifts.begin(), shifts.end(), [](Float_T v) { return v == 0.0; })) 
    {
        const std::vector<Float_T>& scalingPerOutput = scalingCell->getScaling()
                                                                   .getFloatingPointScaling()
                                                                   .getScalingPerOutput();

        auto grandParentsCells = parentElemWiseCell->getParentsCells();
        for(auto grandParentCell: grandParentsCells) {
            auto grandParentScalingCell = Registrar<ScalingCell>::create<Float_T>(getCellModelType(*grandParentCell))
                                            (mDeepNet, grandParentCell->getName() + "_rescale_elemwise", 
                                             grandParentCell->getNbOutputs(), 
                                             Scaling::floatingPointScaling(scalingPerOutput));

            mDeepNet.addCellBetween(grandParentScalingCell, grandParentCell, parentElemWiseCell);
        }

        mDeepNet.removeCell(scalingCell);
    }
}

void N2D2::DeepNetQuantization::quantizeNormalizedNetwork(std::size_t nbBits, 
                                                          ScalingMode actScalingMode) 
{
    const std::vector<std::vector<std::string>>& layers = mDeepNet.getLayers();
    for (auto itLayer = layers.begin() + 1; itLayer != layers.end(); ++itLayer) {
        for (auto itCell = itLayer->begin(); itCell != itLayer->end(); ++itCell) {
            std::shared_ptr<Cell> cell = mDeepNet.getCell(*itCell);
            std::shared_ptr<Cell_Frame_Top> cellFrame = std::dynamic_pointer_cast<Cell_Frame_Top>(cell);
            if(!cell || !cellFrame) {
                throw std::runtime_error("Invalid cell.");
            }

            if(cell->getType() == ScalingCell::Type) {
                const ScalingMode scalingCellMode = (actScalingMode == ScalingMode::FLOAT_MULT)?
                                                        ScalingMode::FLOAT_MULT:ScalingMode::FIXED_MULT;
                approximateScalingCell(dynamic_cast<ScalingCell&>(*cell), scalingCellMode, nbBits);
            }

            const std::shared_ptr<Activation>& activation = cellFrame->getActivation();
            if(!activation) {
                continue;
            }

            quantizeActivationScaling(*cell, *activation, nbBits, actScalingMode);

            approximateActivationScaling(*cell, *activation, actScalingMode);

            // Must come after approximateActivationScaling as the approximation may modify the weights
            // if the actScalingMode is SINGLE_SHIFT or DOUBLE_SHIFT
            quantizeFreeParemeters(*cell, nbBits);
        }
    }
}

void N2D2::DeepNetQuantization::approximateScalingCell(ScalingCell& cell, ScalingMode scalingCellMode, 
                                                       std::size_t nbBits) 
{
    assert(cell.getScaling().getMode() == ScalingMode::FLOAT_MULT);
    if(scalingCellMode == ScalingMode::FLOAT_MULT) {
        return;
    }

    if(scalingCellMode != ScalingMode::FIXED_MULT) {
        throw std::runtime_error("THe scaling cell can only be approximated by a fixed-point approximation.");
    }

    const std::vector<Float_T>& floatScalingPerOutput = cell.getScaling()
                                                            .getFloatingPointScaling()
                                                            .getScalingPerOutput();

    const std::size_t nbFractionalBits = std::min(nbBits*2, (std::size_t) 24);

    std::vector<std::int32_t> fixedScalingPerOutput(floatScalingPerOutput.size());
    for(std::size_t o = 0; o < floatScalingPerOutput.size(); o++) {
        fixedScalingPerOutput[o] = std::round(floatScalingPerOutput[o]*std::pow(2, nbFractionalBits));
    }
    
    cell.setScaling(Scaling::fixedPointScaling(nbFractionalBits, fixedScalingPerOutput));
}

std::pair<std::vector<unsigned char>, double> N2D2::DeepNetQuantization::approximateScalingWithPowerOf2Divs(
                                                        Float_T scaling, std::size_t nbDivisions) 
{
    static const double ROUNDING_THRESHOLD = 0.98;

    assert(nbDivisions > 0);
    assert(scaling <= 1.0);


    double precision = 0.0;

    std::vector<unsigned char> powerOf2Divs(nbDivisions);
    for(std::size_t iDiv = 0; iDiv < nbDivisions; iDiv++) {
        if(precision == 1.0) {
            powerOf2Divs[iDiv-1]++;
            powerOf2Divs[iDiv] = powerOf2Divs[iDiv-1];
        }
        else {
            const std::size_t exponent = std::ceil(std::log2(1.0/(scaling*(1.0 - precision))));
            precision += 1.0/(scaling*std::pow(2, exponent));

            powerOf2Divs[iDiv] = static_cast<unsigned char>(exponent);
        }
    }

    assert(precision <= 1.0);

    if(precision >= ROUNDING_THRESHOLD) {
        precision = 1.0;
    }
    else if(precision < 1.0) {
        precision += 1.0/(scaling*std::pow(2, powerOf2Divs.back()));
        powerOf2Divs.back() = powerOf2Divs.back() - 1;
    }

    assert(precision >= 1.0);

    return std::make_pair(powerOf2Divs, precision);
}

std::vector<std::vector<unsigned char>> N2D2::DeepNetQuantization::approximateActivationScalingWithPowerOf2Divs(Cell& cell, 
                                                        const std::vector<Float_T>& scalingPerOutput, 
                                                        std::size_t nbDivisions)
{
    std::vector<std::vector<unsigned char>> exponentsPerOutput(cell.getNbOutputs());
    for(std::size_t output = 0; output < cell.getNbOutputs(); output++) {
        Float_T rescaleOutputsBy;
        if(nbDivisions == 1) {
            const auto singleDivApprox = approximateScalingWithPowerOf2Divs(scalingPerOutput[output], 1);

            exponentsPerOutput[output] = std::move(singleDivApprox.first);
            rescaleOutputsBy = 1/singleDivApprox.second;
        }
        else if(nbDivisions == 2) {
            const auto doubleDivApprox = approximateScalingWithPowerOf2Divs(scalingPerOutput[output], 2);

            exponentsPerOutput[output] = std::move(doubleDivApprox.first);
            rescaleOutputsBy = 1/doubleDivApprox.second;
        }
        else {
            throw std::runtime_error("Currently only an approximation with 1 or 2 divisions is supported.");
        }

        // Rescale the weights and biasses of the cell to compensate the lost precision
        // of the approximation.
        cell.processFreeParametersPerOutput([&](Float_T d){ 
                                                return rescaleOutputsBy*d; 
                                            }, output);
    }

    return exponentsPerOutput;
}

double N2D2::DeepNetQuantization::getCellThreshold(const std::string& cellName,
                                       const std::unordered_map<std::string, Histogram>& outputsHistogram,
                                       const std::unordered_map<std::string, RangeStats>& outputsRange,
                                       std::size_t nbBits, ClippingMode actClippingMode) 
{
    switch(actClippingMode) {
        case ClippingMode::KL_DIVERGENCE:
            return outputsHistogram.at(cellName).calibrateKLDivergence(nbBits);
        case ClippingMode::MSE:
            return outputsHistogram.at(cellName).calibrateMSE(nbBits);
        default: {
            const auto& range = outputsRange.at(cellName);
            return Utils::max_abs(range.minVal(), range.maxVal());
        }
    }
}

void N2D2::DeepNetQuantization::approximateActivationScaling(Cell& cell, Activation& activation,
                                                             ScalingMode actScalingMode) 
{
    assert(activation.getActivationScaling().getMode() == ScalingMode::FLOAT_MULT);

    const std::vector<Float_T>& scalingPerOutput = activation.getActivationScaling()
                                                             .getFloatingPointScaling()
                                                             .getScalingPerOutput();
    if(actScalingMode == ScalingMode::FLOAT_MULT) {
        // Nothing to do.
    }
    else if(actScalingMode == ScalingMode::FIXED_MULT) {
        /**
         * Find the highest nbFractionalBits so that the scaling 
         * 'std::round(sc * (1ull << nbFractionalBits)' of each output
         * can be stored in an int32_t an thus in scalingFixedPoint.
         * 
         * TODO With unsigned activation like ReLU we could use the maximum
         * of an uint32_t to gain a bit more precision.
         */
        const std::uint64_t limit = std::numeric_limits<std::int32_t>::max();
        const std::size_t maxNbFractionalBits = 50;

        const double maxScaling = *std::max_element(scalingPerOutput.begin(), scalingPerOutput.end());
        std::size_t nbFractionalBits = 32 - 1 - std::ceil(maxScaling);

        assert(std::round(maxScaling * (1ull << nbFractionalBits)) < limit);
        while(std::round(maxScaling * (1ull << (nbFractionalBits + 1))) < limit && 
              nbFractionalBits + 1 <= maxNbFractionalBits) 
        {
            nbFractionalBits++;
        }
        
        

        std::vector<std::int32_t> scalingFixedPoint;
        for(auto sc: scalingPerOutput) {
            scalingFixedPoint.push_back(std::round(sc * (1ull << nbFractionalBits)));
        }

        activation.setActivationScaling(Scaling::fixedPointScaling(nbFractionalBits, scalingFixedPoint));
    }
    else if(actScalingMode == ScalingMode::SINGLE_SHIFT) {
        std::vector<unsigned char> shifts;
        for(const auto& powOf2Exponents: approximateActivationScalingWithPowerOf2Divs(cell, scalingPerOutput, 1)) {
            assert(powOf2Exponents.size() == 1);
            shifts.push_back(powOf2Exponents[0]);
        }

        activation.setActivationScaling(Scaling::singleShiftScaling(shifts));
    }
    else if(actScalingMode == ScalingMode::DOUBLE_SHIFT) {
        std::vector<std::pair<unsigned char, unsigned char>> shifts;
        for(const auto& powOf2Exponents: approximateActivationScalingWithPowerOf2Divs(cell, scalingPerOutput, 2)) {
            assert(powOf2Exponents.size() == 2);
            assert(powOf2Exponents[0] <= powOf2Exponents[1]);
            shifts.push_back({powOf2Exponents[1] - powOf2Exponents[0], powOf2Exponents[1]});
        }

        activation.setActivationScaling(Scaling::doubleShiftScaling(shifts));
    }
    else {
        throw std::runtime_error("Unsupported scaling mode.");
    }
}

void  N2D2::DeepNetQuantization::quantizeActivationScaling(Cell& cell, Activation& activation, 
                                                           std::size_t nbBits, 
                                                           ScalingMode actScalingMode) 
{
    const ScalingMode scalingMode = activation.getActivationScaling().getMode();
    if(scalingMode != ScalingMode::FLOAT_MULT) {
        assert(scalingMode == ScalingMode::NONE);
        return;
    }

    const Float_T unsignedMax = std::pow(2, nbBits) - 1;
    const Float_T signedMax = std::pow(2, nbBits - 1) - 1;
    
    std::vector<Float_T> scalingPerOutput = activation.getActivationScaling()
                                                      .getFloatingPointScaling()
                                                      .getScalingPerOutput();
    for(Float_T& scaling: scalingPerOutput) {
        const std::string activationType = activation.getType();
        if(activationType == RectifierActivation::Type) {
            scaling /= DeepNetExport::isCellInputsUnsigned(cell)?
                            signedMax*unsignedMax/unsignedMax:
                            signedMax*signedMax/unsignedMax;
        }
        else if(activationType == LogisticActivation::Type || 
                activationType == LogisticActivation::TypeWithLoss) 
        {
            scaling /= 2*(DeepNetExport::isCellInputsUnsigned(cell)?
                            signedMax*unsignedMax/signedMax:
                            signedMax*signedMax/signedMax);
        }
        else {
            scaling /= DeepNetExport::isCellInputsUnsigned(cell)?
                            signedMax*unsignedMax/signedMax:
                            signedMax*signedMax/signedMax;
        }
    }

    // Ensure that every scalingPerOutput[o] <= 1.0 so that we only have to manage 
    // downscalling when approximating the rescaling. A scalingPerOutput[o] > 1.0 should
    // be really rare
    // TODO Find a network where it happens and test how well it works
    const Float_T maxScaling = *std::max_element(scalingPerOutput.begin(), scalingPerOutput.end());
    if(maxScaling > 1.0 && (actScalingMode == ScalingMode::SINGLE_SHIFT || 
                            actScalingMode == ScalingMode::DOUBLE_SHIFT))
    {
        for(Float_T& scaling: scalingPerOutput) {
            scaling /= maxScaling;
        }
    }

    activation.setActivationScaling(
        Scaling::floatingPointScaling(std::move(scalingPerOutput))
    );
}

void N2D2::DeepNetQuantization::quantizeFreeParemeters(Cell& cell, std::size_t nbBits) {
    cell.processFreeParameters([&](Float_T wt) { 
        const double scaling = (double) std::pow(2, nbBits - 1) - 1;
        return std::round(wt*scaling);
    }, Cell::Multiplicative);

    cell.processFreeParameters([&](Float_T bias) { 
        // For the bias we also need to scale it by the maximum value of the input type.
        // A bias is just like an extra connection where the input is equal to 1.0.
        
        double scaling = (double) std::pow(2, nbBits - 1) - 1;
        if(DeepNetExport::isCellInputsUnsigned(cell)) {
            scaling *= std::pow(2, nbBits) - 1;
        }
        else {
            scaling *= std::pow(2, nbBits - 1) - 1;
        }
        
        return std::round(bias*scaling);
    }, Cell::Additive);
}

std::string N2D2::DeepNetQuantization::getCellModelType(const Cell& cell) {
    const Cell_Frame_Top& cellFrameTop = dynamic_cast<const Cell_Frame_Top&>(cell);
    if(cellFrameTop.isCuda()) {
        return Cell_Frame_Top::FRAME_CUDA;
    }
    else {
        return Cell_Frame_Top::FRAME;
    }
}

void N2D2::DeepNetQuantization::errorIfCellExist(const std::string& cellName) const {
    if(mDeepNet.hasCell(cellName)) {
        throw std::runtime_error("Can't add the cell " + cellName + ". "
                                 "A cell with the same name already exist.");
    }
}
