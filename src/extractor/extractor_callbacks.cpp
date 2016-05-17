#include "extractor/extractor_callbacks.hpp"
#include "extractor/extraction_containers.hpp"
#include "extractor/extraction_node.hpp"
#include "extractor/extraction_way.hpp"

#include "extractor/external_memory_node.hpp"
#include "extractor/restriction.hpp"
#include "util/simple_logger.hpp"
#include "util/for_each_pair.hpp"

#include <boost/optional/optional.hpp>

#include <osmium/osm.hpp>

#include "osrm/coordinate.hpp"

#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace osrm
{
namespace extractor
{

ExtractorCallbacks::ExtractorCallbacks(ExtractionContainers &extraction_containers)
    : external_memory(extraction_containers)
{
    string_map[""] = 0;
}

/**
 * Takes the node position from osmium and the filtered properties from the lua
 * profile and saves them to external memory.
 *
 * warning: caller needs to take care of synchronization!
 */
void ExtractorCallbacks::ProcessNode(const osmium::Node &input_node,
                                     const ExtractionNode &result_node)
{
    external_memory.all_nodes_list.push_back(
        {util::toFixed(util::FloatLongitude(input_node.location().lon())),
         util::toFixed(util::FloatLatitude(input_node.location().lat())),
         OSMNodeID(input_node.id()), result_node.barrier, result_node.traffic_lights});
}

void ExtractorCallbacks::ProcessRestriction(
    const boost::optional<InputRestrictionContainer> &restriction)
{
    if (restriction)
    {
        external_memory.restrictions_list.push_back(restriction.get());
        // util::SimpleLogger().Write() << "from: " << restriction.get().restriction.from.node <<
        //                           ",via: " << restriction.get().restriction.via.node <<
        //                           ", to: " << restriction.get().restriction.to.node <<
        //                           ", only: " << (restriction.get().restriction.flags.is_only ?
        //                           "y" : "n");
    }
}
/**
 * Takes the geometry contained in the ```input_way``` and the tags computed
 * by the lua profile inside ```parsed_way``` and computes all edge segments.
 *
 * Depending on the forward/backwards weights the edges are split into forward
 * and backward edges.
 *
 * warning: caller needs to take care of synchronization!
 */
void ExtractorCallbacks::ProcessWay(const osmium::Way &input_way, const ExtractionWay &parsed_way)
{
    if ((parsed_way.forward_travel_mode == TRAVEL_MODE_INACCESSIBLE &&
        parsed_way.backward_travel_mode == TRAVEL_MODE_INACCESSIBLE) ||
        (parsed_way.forward_speed <= 0 && parsed_way.backward_speed <= 0 && parsed_way.duration <= 0) ||
        (parsed_way.forward_weight_per_meter <= 0 && parsed_way.backward_weight_per_meter <= 0 && parsed_way.weight <= 0))
    { // Only true if the way is specified by the speed profile
        return;
    }

    if (input_way.nodes().size() <= 1)
    { // safe-guard against broken data
        return;
    }

    if (std::numeric_limits<decltype(input_way.id())>::max() == input_way.id())
    {
        util::SimpleLogger().Write(logDEBUG) << "found bogus way with id: " << input_way.id()
                                             << " of size " << input_way.nodes().size();
        return;
    }

    InternalExtractorEdge::DurationData forward_duration_data;
    InternalExtractorEdge::DurationData backward_duration_data;
    InternalExtractorEdge::WeightData forward_weight_data;
    InternalExtractorEdge::WeightData backward_weight_data;

    const auto toValueByEdgeOrByMeter = [&input_way](const double by_way, const double by_meter) -> detail::ByEdgeOrByMeterValue{
        if (by_way > 0)
        {
            // FIXME We devide by the numer of edges here, but should rather consider
            // the length of each segment. We would eigther have to compute the length
            // of the whole way here (we can't: no node coordinates) or push that back
            // to the container and keep a reference to the way.
            const unsigned num_edges = (input_way.nodes().size() - 1);
            return detail::ValueByEdge {by_way / num_edges};
        }
        else
        {
            return detail::ValueByMeter {by_meter};
        }
    };

    if (parsed_way.forward_travel_mode != TRAVEL_MODE_INACCESSIBLE)
    {
        BOOST_ASSERT(parsed_way.duration > 0 || parsed_way.forward_speed > 0);
        forward_duration_data = toValueByEdgeOrByMeter(parsed_way.duration, parsed_way.forward_speed / 3.6);
        BOOST_ASSERT(parsed_way.weight > 0 || parsed_way.forward_weight_by_meters > 0);
        forward_weight_data = toValueByEdgeOrByMeter(parsed_way.weight, parsed_way.forward_weight_per_meter);
    }
    if (parsed_way.backward_travel_mode != TRAVEL_MODE_INACCESSIBLE)
    {
        BOOST_ASSERT(parsed_way.duration > 0 || parsed_way.backward_speed > 0);
        backward_duration_data = toValueByEdgeOrByMeter(parsed_way.duration, parsed_way.backward_speed / 3.6);
        BOOST_ASSERT(parsed_way.weight > 0 || parsed_way.backward_weight_per_meter > 0);
        backward_weight_data = toValueByEdgeOrByMeter(parsed_way.weight, parsed_way.backward_weight_per_meter);
    }

    // FIXME this need to be moved into the profiles
    const char *data = input_way.get_value_by_key("highway");
    guidance::RoadClassificationData road_classification;
    if (data)
    {
        road_classification.road_class = guidance::functionalRoadClassFromTag(data);
    }

    // Get the unique identifier for the street name
    const auto &string_map_iterator = string_map.find(parsed_way.name);
    unsigned name_id = external_memory.name_lengths.size();
    if (string_map.end() == string_map_iterator)
    {
        auto name_length = std::min<unsigned>(255u, parsed_way.name.size());
        std::copy(parsed_way.name.c_str(), parsed_way.name.c_str() + name_length,
                  std::back_inserter(external_memory.name_char_data));
        external_memory.name_lengths.push_back(name_length);
        string_map.insert(std::make_pair(parsed_way.name, name_id));
    }
    else
    {
        name_id = string_map_iterator->second;
    }

    const bool split_edge = (parsed_way.forward_speed > 0) &&
                            (TRAVEL_MODE_INACCESSIBLE != parsed_way.forward_travel_mode) &&
                            (parsed_way.backward_speed > 0) &&
                            (TRAVEL_MODE_INACCESSIBLE != parsed_way.backward_travel_mode) &&
                            ((parsed_way.forward_weight_per_meter != parsed_way.backward_weight_per_meter) ||
                             (parsed_way.forward_speed != parsed_way.backward_speed) ||
                             (parsed_way.forward_travel_mode != parsed_way.backward_travel_mode));

    std::transform(input_way.nodes().begin(), input_way.nodes().end(),
                   std::back_inserter(external_memory.used_node_id_list),
                   [](const osmium::NodeRef &ref)
                   {
                       return OSMNodeID(ref.ref());
                   });

    const bool is_opposite_way = TRAVEL_MODE_INACCESSIBLE == parsed_way.forward_travel_mode;

    // traverse way in reverse in this case
    if (is_opposite_way)
    {
        BOOST_ASSERT(split_edge == false);
        BOOST_ASSERT(parsed_way.backward_travel_mode != TRAVEL_MODE_INACCESSIBLE);
        util::for_each_pair(
            input_way.nodes().crbegin(), input_way.nodes().crend(),
            [&](const osmium::NodeRef &first_node, const osmium::NodeRef &last_node)
            {
                external_memory.all_edges_list.push_back(InternalExtractorEdge{
                    {OSMNodeID(first_node.ref()), OSMNodeID(last_node.ref()), name_id,
                     INVALID_EDGE_WEIGHT, true, false, parsed_way.roundabout,
                     parsed_way.is_access_restricted, parsed_way.is_startpoint,
                     parsed_way.backward_travel_mode, false, road_classification},
                    backward_weight_data,
                    backward_duration_data,
                    {}});
            });

        external_memory.way_start_end_id_list.push_back(
            {OSMWayID(input_way.id()), OSMNodeID(input_way.nodes().back().ref()),
             OSMNodeID(input_way.nodes()[input_way.nodes().size() - 2].ref()),
             OSMNodeID(input_way.nodes()[1].ref()), OSMNodeID(input_way.nodes()[0].ref())});
    }
    else
    {
        const bool forward_only =
            split_edge || TRAVEL_MODE_INACCESSIBLE == parsed_way.backward_travel_mode;
        util::for_each_pair(
            input_way.nodes().cbegin(), input_way.nodes().cend(),
            [&](const osmium::NodeRef &first_node, const osmium::NodeRef &last_node)
            {
                external_memory.all_edges_list.push_back(InternalExtractorEdge{
                    {OSMNodeID(first_node.ref()), OSMNodeID(last_node.ref()), name_id,
                     INVALID_EDGE_WEIGHT, true, !forward_only, parsed_way.roundabout,
                     parsed_way.is_access_restricted, parsed_way.is_startpoint,
                     parsed_way.forward_travel_mode, split_edge, road_classification},
                    forward_weight_data,
                    forward_duration_data,
                    {}});
            });
        if (split_edge)
        {
            BOOST_ASSERT(parsed_way.backward_travel_mode != TRAVEL_MODE_INACCESSIBLE);
            util::for_each_pair(
                input_way.nodes().cbegin(), input_way.nodes().cend(),
                [&](const osmium::NodeRef &first_node, const osmium::NodeRef &last_node)
                {
                    external_memory.all_edges_list.push_back(InternalExtractorEdge{
                        {OSMNodeID(first_node.ref()), OSMNodeID(last_node.ref()), name_id,
                         INVALID_EDGE_WEIGHT, false, true, parsed_way.roundabout,
                         parsed_way.is_access_restricted, parsed_way.is_startpoint,
                         parsed_way.backward_travel_mode, split_edge, road_classification},
                        backward_weight_data,
                        backward_duration_data,
                        {}});
                });
        }

        external_memory.way_start_end_id_list.push_back(
            {OSMWayID(input_way.id()), OSMNodeID(input_way.nodes().back().ref()),
             OSMNodeID(input_way.nodes()[input_way.nodes().size() - 2].ref()),
             OSMNodeID(input_way.nodes()[1].ref()), OSMNodeID(input_way.nodes()[0].ref())});
    }
}
}
}
