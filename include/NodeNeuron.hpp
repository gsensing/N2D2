/*
    (C) Copyright 2010 CEA LIST. All Rights Reserved.
    Contributor(s): Olivier BICHLER (olivier.bichler@cea.fr)
                    Damien QUERLIOZ (damien.querlioz@cea.fr)

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

#ifndef N2D2_NODENEURON_H
#define N2D2_NODENEURON_H

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "Node.hpp"
#include "utils/Parameterizable.hpp"

namespace N2D2 {

class Synapse;

/**
 * Abstract neuron base class, which provides the minimum interface required for
 * a neuron. Any neuron model must inherit from
 * this class.
*/
class NodeNeuron : public Node, public Parameterizable {
public:
    /**
     * Constructor.
     *
     * @param net           Network object associated to the simulation
    */
    NodeNeuron(Network& net);

    /**
     * Add a link to an input neuron. This method creates a Synapse.
     *
     * @param origin        Origin node to connect to
    */

    virtual void addLink(Node* origin);

    /**
     * Add a lateral branch. This is a node that should be inhibited through
     *lateral inhibition upon activation of the neuron.
     *
     * @param lateralBranch Node to inhibit upon activiation of the neuron
    */
    virtual void addLateralBranch(NodeNeuron* lateralBranch);

    /**
     * Method receiving the input events of the node, after having passed
     *through the synaptic connection. These events were
     * generated by propagateSpike().
     * This method is not intended to be called directly.
     *
     * @param link          Input node address
     * @param timestamp     Timestamp of the event
     * @param type          Event type
    */
    virtual void
    incomingSpike(Node* link, Time_T timestamp, EventType_T type = 0) = 0;

    /**
     * Method handling internal events of the node and generating output events.
     * This method is not intended to be called directly.
     *
     * @param timestamp     Timestamp of the event
     * @param type          Event type
    */
    virtual void emitSpike(Time_T timestamp, EventType_T type = 0) = 0;

    /**
     * Method called when a lateral node was activated, used to implement
     * lateral inhibition.
     * This method is not intended to be called directly.
    */
    virtual void lateralInhibition(Time_T /*timestamp*/,
                                   EventType_T /*type*/ = 0) {};

    /**
     * Reset the state of the node.
    */
    virtual void reset(Time_T timestamp = 0) = 0;

    void notify(Time_T timestamp, NotifyType notify);

    /**
     * The node emits the spikes from the @p activity vector without changing
     *its state.
     * This is used to emulate the output of the neuron with a pre-computed or
     *previously saved result.
     *
     * @param activity      List of events to be emitted by the node
    */
    void readActivity(const std::vector<Time_T>& activity);

    /**
     * Save the entire state of the neuron.
     *
     * @param dirName       Directory to put the neuron state files
    */
    void save(const std::string& dirName) const;

    /**
     * Load the entire state of the neuron.
     *
     * @param dirName       Directory to load the neuron state files from
    */
    void load(const std::string& dirName);
    virtual void logWeights(const std::string& fileName) const;
    void logState(const std::string& fileName = "",
                  bool append = false,
                  bool plot = true);

    /**
     * Appends the synaptic stats for all the synapses (dependent on the
     *synaptic model: number of read or write events, energy
     * consumed, etc) to a file stream.
     *
     * @param   dataFile        File stream to append the stats to
     * @param   suffix          String to append at the end of each stat line
     *(which is used to add the Xcell ID, Layer ID...)
    */
    void logStats(std::ofstream& dataFile, const std::string& suffix) const;

    /// Clear the synaptic stats for all the synapses
    void clearStats();
    cv::Mat reconstructPattern(bool normalize = false, bool multiLayer = true);
    cv::Mat reconstructActivity(Time_T start = 0,
                                Time_T stop = 0,
                                EventType_T type = 0,
                                bool order = false,
                                bool normalize = true);

    unsigned int getNbLinks() const
    {
        return mLinks.size();
    };

    /// Destructor.
    virtual ~NodeNeuron();

protected:
    /**
     * Initialization routine called when Network::run() is called for the first
     * time.
     * This method is not intended to be called directly.
    */
    virtual void initialize() = 0;

    /**
     * Finalization routine called at the end of Network::run().
     * This method is not intended to be called directly.
    */
    virtual void finalize() {};

    virtual Synapse* newSynapse() const = 0;
    virtual void saveInternal(std::ofstream& /*dataFile*/) const {};
    virtual void loadInternal(std::ifstream& /*dataFile*/) {};
    virtual void logStatePlot() = 0;

    // Internal variables
    /**
     * Indicates whether the state of the neuron was already initialized or not
    */
    bool mInitializedState;
    /**
     * Vector containing the lateral branches of the neurons, that should be
     * inhibited through lateral inhibition upon
     * activation of the neuron
    */
    std::vector<NodeNeuron*> mLateralBranches;
    /// Map containing the synapses of the neuron, associated to their input
    /// neurons
    std::unordered_map<Node*, Synapse*> mLinks;
    /// File stream to store the state of the neuron
    std::ofstream mStateLog; // Note: using fstream makes this class
    // automatically non-copyable
    /// Name of the file storing the state of the neuron
    std::string mStateLogFile;
    /// Indicates if the state of the neuron must be logged or not
    bool mStateLogPlot;
    /// Indicates if the weight reconstruction images stored in cache are still
    /// valid or not
    bool mCacheValid;
};
}

#endif // N2D2_NODENEURON_H
