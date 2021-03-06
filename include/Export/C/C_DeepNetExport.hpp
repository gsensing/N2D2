/*
    (C) Copyright 2016 CEA LIST. All Rights Reserved.
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

#ifndef N2D2_C_DEEPNETEXPORT_H
#define N2D2_C_DEEPNETEXPORT_H

#include "Export/C/C_CellExport.hpp"
#include "Export/DeepNetExport.hpp"

namespace N2D2 {
class C_DeepNetExport : public DeepNetExport {
public:
    static void generate(DeepNet& deepNet, const std::string& dirName);

    static void generateParamsHeader(const std::string& fileName);
    static void generateEnvironmentHeader(DeepNet& deepNet,
                                          const std::string& fileName);
    static void generateDeepNetHeader(DeepNet& deepNet,
                                      const std::string& name,
                                      const std::string& fileName);
    static void generateDeepNetProgram(DeepNet& deepNet,
                                       const std::string& name,
                                       const std::string& fileName);

    static void generateHeaderBegin(DeepNet& deepNet,
                                    std::ofstream& header,
                                    const std::string& fileName);
    static void generateHeaderIncludes(DeepNet& deepNet, std::ofstream& header);
    static void generateHeaderConstants(DeepNet& deepNet,
                                        std::ofstream& header);
    static void generateHeaderFunction(DeepNet& deepNet,
                                       const std::string& name,
                                       std::ofstream& header);
    static void generateHeaderEnd(DeepNet& deepNet, std::ofstream& header);

    static void generateProgramBegin(DeepNet& deepNet, std::ofstream& prog);
    static void generateProgramData(DeepNet& deepNet, std::ofstream& prog);
    static void generateProgramFunction(DeepNet& deepNet,
                                        const std::string& name,
                                        std::ofstream& prog);

private:
    static Registrar<DeepNetExport> mRegistrar;
};
}

#endif // N2D2_C_DEEPNETEXPORT_H
