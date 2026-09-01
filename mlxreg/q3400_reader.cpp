/*
 * Minimal Q3400 persistent PPCNT reader core.
 *
 * The command-line interface in this file is intentionally temporary.  The
 * useful boundary is Mapping::select() + Reader::read_once(); final mlxlink-
 * compatible command-line semantics are deliberately left for a later stage.
 */

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <mtcr.h>
#include "mlxreg/mlxreg_lib/mlxreg_lib.h"
#include "mlxreg/mlxreg_lib/mlxreg_parser.h"
#include "pddr_module_snapshot.h"

using namespace std;
using namespace mlxreg;
using Clock = chrono::steady_clock;

namespace
{

const uint32_t PPCNT_GROUP = 0x16;
const uint32_t HISTOGRAM_GROUP = 0x23;
const unsigned MAX_HISTOGRAM_BINS = 19;
const unsigned MAX_LOCAL_PORT = 254;

class ObjectReadError : public runtime_error
{
public:
    explicit ObjectReadError(const string& message) : runtime_error(message) {}
};

const map<string, string> PCICONF_TO_BDF = {
    {"pciconf0", "03:00.0"},
    {"pciconf1", "04:00.0"},
    {"pciconf2", "05:00.0"},
    {"pciconf3", "09:00.0"},
};

string trim(const string& value)
{
    const string whitespace = " \t\r\n";
    const size_t first = value.find_first_not_of(whitespace);
    if (first == string::npos)
        return "";
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

vector<string> split(const string& value, char delimiter, bool keep_empty = false)
{
    vector<string> result;
    string item;
    istringstream input(value);
    while (getline(input, item, delimiter))
    {
        if (keep_empty || !item.empty())
            result.push_back(item);
    }
    if (keep_empty && !value.empty() && value[value.size() - 1] == delimiter)
        result.push_back("");
    return result;
}

unsigned parse_unsigned(const string& text,
                        const string& name,
                        unsigned minimum,
                        unsigned maximum)
{
    if (text.empty() || text[0] == '-' || text[0] == '+')
        throw invalid_argument(name + " must be an unsigned decimal integer");

    errno = 0;
    char* end = nullptr;
    const unsigned long long value = strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
        value < minimum || value > maximum)
    {
        ostringstream error;
        error << name << " must be in range " << minimum << ".." << maximum;
        throw invalid_argument(error.str());
    }
    return static_cast<unsigned>(value);
}

void require_readable_file(const string& path, const string& name)
{
    struct stat status;
    if (path.empty() || stat(path.c_str(), &status) != 0)
        throw invalid_argument(name + " does not exist: " + path);
    if (!S_ISREG(status.st_mode) || status.st_size <= 0)
        throw invalid_argument(name + " must be a non-empty regular file: " + path);
    if (access(path.c_str(), R_OK) != 0)
        throw invalid_argument(name + " is not readable: " + path);
}

struct MappingEntry
{
    string label_port;
    unsigned ipil = 0;
    unsigned channel = 0;
    unsigned mapping_channel = 0;
    unsigned split = 0;
    string pciconf;
    string bdf;
    unsigned local_port = 0;
    string module;
    string sub_module;
    size_t source_line = 0;
};

unsigned parse_channel(const string& value, size_t line)
{
    string number = trim(value);
    if (number.size() > 2 &&
        (number[0] == 'C' || number[0] == 'c') &&
        (number[1] == 'H' || number[1] == 'h'))
        number = number.substr(2);
    return parse_unsigned(number, "channel at mapping line " + to_string(line), 1, 8);
}

string normalize_label_port(const string& value, size_t line)
{
    const string label = trim(value);
    const unsigned number = parse_unsigned(label,
                                           "label_port at mapping line " + to_string(line),
                                           1, 65535);
    return to_string(number);
}

class Mapping
{
public:
    explicit Mapping(const string& path)
    {
        require_readable_file(path, "mapping");
        ifstream input(path.c_str());
        string line;
        if (!getline(input, line))
            throw invalid_argument("mapping has no header: " + path);

        vector<string> header = split(line, '\t', true);
        for (size_t i = 0; i < header.size(); ++i)
            _columns[trim(header[i])] = i;

        static const char* required[] = {
            "label_port", "ipil", "channel", "split", "pciconf",
            "local_port", "module", "sub_module"
        };
        for (const char* column : required)
        {
            if (_columns.count(column) == 0)
                throw invalid_argument("mapping is missing required column: " + string(column));
        }

        size_t line_number = 1;
        set<pair<string, unsigned> > identities;
        while (getline(input, line))
        {
            ++line_number;
            if (trim(line).empty())
                continue;
            vector<string> fields = split(line, '\t', true);
            if (fields.size() != header.size())
                throw invalid_argument("mapping line " + to_string(line_number) +
                                       " has " + to_string(fields.size()) +
                                       " columns; expected " + to_string(header.size()));

            MappingEntry entry;
            entry.label_port = normalize_label_port(value(fields, "label_port"), line_number);
            entry.ipil = parse_unsigned(value(fields, "ipil"),
                                        "ipil at mapping line " + to_string(line_number), 1, 2);
            entry.mapping_channel = parse_channel(value(fields, "channel"), line_number);
            if (entry.mapping_channel > 4)
                throw invalid_argument("mapping channel must be 1..4 at line " +
                                       to_string(line_number));
            entry.channel = (entry.ipil - 1) * 4 + entry.mapping_channel;
            entry.split = parse_unsigned(value(fields, "split"),
                                         "split at mapping line " + to_string(line_number),
                                         0, 255);
            entry.pciconf = value(fields, "pciconf");
            map<string, string>::const_iterator device = PCICONF_TO_BDF.find(entry.pciconf);
            if (device == PCICONF_TO_BDF.end())
                throw invalid_argument("invalid pciconf at mapping line " +
                                       to_string(line_number) + ": " + entry.pciconf);
            entry.bdf = device->second;
            entry.local_port = parse_unsigned(value(fields, "local_port"),
                                               "local_port at mapping line " + to_string(line_number),
                                               0, MAX_LOCAL_PORT);
            entry.module = value(fields, "module");
            entry.sub_module = value(fields, "sub_module");
            entry.source_line = line_number;

            const pair<string, unsigned> identity(entry.label_port, entry.channel);
            if (!identities.insert(identity).second)
                throw invalid_argument("duplicate mapping for " + entry.label_port +
                                       " channel " + to_string(entry.channel));
            _entries.push_back(entry);
            _ports.insert(entry.label_port);
        }

        if (_entries.empty())
            throw invalid_argument("mapping contains no telemetry objects");
    }

    const vector<MappingEntry>& entries() const { return _entries; }

    vector<MappingEntry> select(const set<string>& ports,
                                const set<unsigned>& channels) const
    {
        for (const string& port : ports)
        {
            if (_ports.count(port) == 0)
                throw invalid_argument("selected port does not exist in mapping: " + port);
        }

        if (!channels.empty())
        {
            const set<string>& ports_to_check = ports.empty() ? _ports : ports;
            for (const string& port : ports_to_check)
            {
                for (unsigned channel : channels)
                {
                    bool found = false;
                    for (const MappingEntry& entry : _entries)
                        found = found || (entry.label_port == port && entry.channel == channel);
                    if (!found)
                        throw invalid_argument("selected channel " + to_string(channel) +
                                               " does not exist for port " + port);
                }
            }
        }

        vector<MappingEntry> selected;
        for (const MappingEntry& entry : _entries)
        {
            if ((!ports.empty() && ports.count(entry.label_port) == 0) ||
                (!channels.empty() && channels.count(entry.channel) == 0))
                continue;
            selected.push_back(entry);
        }
        if (selected.empty())
            throw invalid_argument("selection contains no telemetry objects");
        return selected;
    }

private:
    string value(const vector<string>& fields, const string& column) const
    {
        return trim(fields.at(_columns.at(column)));
    }

    map<string, size_t> _columns;
    vector<MappingEntry> _entries;
    set<string> _ports;
};

unsigned port_number(const string& port)
{
    return parse_unsigned(port, "front-panel label_port", 1, 65535);
}

set<string> parse_ports(const string& specification)
{
    set<string> ports;
    if (specification.empty() || specification == "all")
        return ports;
    for (const string& raw_item : split(specification, ','))
    {
        const string item = trim(raw_item);
        const size_t dash = item.find('-');
        if (dash == string::npos)
        {
            if (item.empty())
                throw invalid_argument("empty port in selection");
            ports.insert(item);
            continue;
        }
        if (item.find('-', dash + 1) != string::npos)
            throw invalid_argument("invalid port range: " + item);
        const unsigned first = port_number(trim(item.substr(0, dash)));
        const unsigned last = port_number(trim(item.substr(dash + 1)));
        if (first > last)
            throw invalid_argument("descending port range: " + item);
        if (last - first > 4096)
            throw invalid_argument("port range is unreasonably large: " + item);
        for (unsigned current = first; current <= last; ++current)
            ports.insert(to_string(current));
    }
    if (ports.empty())
        throw invalid_argument("empty port selection");
    return ports;
}

set<unsigned> parse_channels(const string& specification)
{
    set<unsigned> channels;
    if (specification.empty() || specification == "all")
        return channels;
    for (const string& item : split(specification, ','))
        channels.insert(parse_channel(trim(item), 0));
    if (channels.empty())
        throw invalid_argument("empty channel selection");
    return channels;
}

struct PpcntValue
{
    uint64_t time_since_last_clear_ms = 0;
    uint64_t received_bits = 0;
    uint64_t symbol_errors = 0;
    uint64_t corrected_bits = 0;
    uint64_t raw_error = 0;
    uint64_t effective_errors = 0;
    uint32_t raw_ber_coefficient = 0;
    uint32_t raw_ber_magnitude = 0;
    uint32_t raw_ber_lane0_coefficient = 0;
    uint32_t raw_ber_lane0_magnitude = 0;
    uint32_t effective_ber_coefficient = 0;
    uint32_t effective_ber_magnitude = 0;
    uint32_t symbol_ber_coefficient = 0;
    uint32_t symbol_ber_magnitude = 0;
};

struct HistogramBin
{
    unsigned index = 0;
    uint32_t low = 0;
    uint32_t high = 0;
    uint64_t count = 0;
};

struct HistogramValue
{
    uint32_t active_type = 0;
    vector<HistogramBin> bins;
};

struct ModulePlan
{
    string label_port;
    vector<MappingEntry> contexts;
};

vector<ModulePlan> build_module_plans(const vector<MappingEntry>& all_entries,
                                      const vector<MappingEntry>& selected)
{
    set<string> selected_ports;
    for (const MappingEntry& entry : selected)
        selected_ports.insert(entry.label_port);

    map<string, vector<MappingEntry> > grouped;
    for (const MappingEntry& entry : all_entries)
    {
        if (selected_ports.count(entry.label_port) != 0)
            grouped[entry.label_port].push_back(entry);
    }

    vector<ModulePlan> plans;
    for (auto& item : grouped)
    {
        sort(item.second.begin(), item.second.end(),
             [](const MappingEntry& a, const MappingEntry& b) {
                 if (a.channel != b.channel) return a.channel < b.channel;
                 if (a.pciconf != b.pciconf) return a.pciconf < b.pciconf;
                 if (a.local_port != b.local_port) return a.local_port < b.local_port;
                 return a.source_line < b.source_line;
             });
        if (item.second.empty())
            throw runtime_error("module plan has no context for label_port " + item.first);
        ModulePlan plan;
        plan.label_port = item.first;
        plan.contexts.swap(item.second);
        plans.push_back(plan);
    }
    sort(plans.begin(), plans.end(), [](const ModulePlan& a, const ModulePlan& b) {
        return port_number(a.label_port) < port_number(b.label_port);
    });
    return plans;
}

struct ObjectResult
{
    MappingEntry topology;
    bool success = false;
    bool counters_requested = false;
    bool counters_success = false;
    bool histogram_requested = false;
    bool histogram_success = false;
    PpcntValue ppcnt;
    HistogramValue histogram;
    string error;
};

struct ModuleResult
{
    string label_port;
    MappingEntry canonical_context;
    MappingEntry actual_context;
    bool success = false;
    bool fallback_used = false;
    q3400::ModuleSnapshotData snapshot;
    string updated_at;
    string error;
};

struct AggregatedCounterSlot
{
    unsigned channel = 0;
    bool available = false;
    PpcntValue value;
};

struct AggregatedHistogramBin
{
    unsigned index = 0;
    uint32_t low = 0;
    uint32_t high = 0;
    vector<bool> occurrence_available;
    vector<uint64_t> occurrences;
};

struct AggregatedHistogram
{
    vector<unsigned> channels;
    bool definition_consistent = true;
    bool active_type_available = false;
    uint32_t active_type = 0;
    vector<AggregatedHistogramBin> bins;
};

struct AggregatedLogicalPort
{
    string name;
    unsigned ipil = 0;
    vector<unsigned> channels;
    vector<q3400::ModuleMediaLane> module_lanes;
    bool counters_requested = false;
    vector<AggregatedCounterSlot> counters;
    bool histogram_requested = false;
    AggregatedHistogram histogram;
};

struct AggregatedPortResult
{
    string label_port;
    bool module_requested = false;
    bool module_result_available = false;
    ModuleResult module_metadata;
    q3400::ModuleSnapshotData module_snapshot;
    vector<AggregatedLogicalPort> logical_ports;
};

struct ScanResult
{
    vector<ObjectResult> objects;
    vector<ModuleResult> modules;
    vector<AggregatedPortResult> aggregated_ports;
    double cold_start_ms = 0.0;
    double channel_scan_ms = 0.0;
    double module_scan_ms = 0.0;
    double scan_ms = 0.0;
    double total_ms = 0.0;
    size_t success_count = 0;
    size_t failed_count = 0;
    size_t module_success_count = 0;
    size_t module_absent_count = 0;
    size_t module_failed_count = 0;
};

void print_scan_json(const ScanResult& result);

const ObjectResult* find_object(const vector<ObjectResult>& objects,
                                const string& label_port,
                                unsigned channel)
{
    for (const ObjectResult& object : objects)
    {
        if (object.topology.label_port == label_port &&
            object.topology.channel == channel)
            return &object;
    }
    return nullptr;
}

const ModuleResult* find_module(const vector<ModuleResult>& modules,
                                const string& label_port)
{
    for (const ModuleResult& module : modules)
    {
        if (module.label_port == label_port)
            return &module;
    }
    return nullptr;
}

AggregatedHistogram aggregate_histogram(const vector<MappingEntry>& entries,
                                        const vector<ObjectResult>& objects)
{
    AggregatedHistogram result;
    for (const MappingEntry& entry : entries)
        result.channels.push_back(entry.channel);

    for (size_t channel_index = 0; channel_index < entries.size(); ++channel_index)
    {
        const ObjectResult* object =
            find_object(objects, entries[channel_index].label_port,
                        entries[channel_index].channel);
        if (!object || !object->histogram_success)
            continue;

        if (!result.active_type_available)
        {
            result.active_type = object->histogram.active_type;
            result.active_type_available = true;
            for (const HistogramBin& source : object->histogram.bins)
            {
                AggregatedHistogramBin bin;
                bin.index = source.index;
                bin.low = source.low;
                bin.high = source.high;
                bin.occurrence_available.resize(entries.size(), false);
                bin.occurrences.resize(entries.size(), 0);
                result.bins.push_back(bin);
            }
        }

        bool same_definition =
            object->histogram.active_type == result.active_type &&
            object->histogram.bins.size() == result.bins.size();
        for (size_t bin_index = 0;
             same_definition && bin_index < result.bins.size(); ++bin_index)
        {
            const HistogramBin& source = object->histogram.bins[bin_index];
            const AggregatedHistogramBin& target = result.bins[bin_index];
            same_definition = source.index == target.index &&
                              source.low == target.low &&
                              source.high == target.high;
        }
        if (!same_definition)
        {
            result.definition_consistent = false;
            continue;
        }
        for (size_t bin_index = 0; bin_index < result.bins.size(); ++bin_index)
        {
            result.bins[bin_index].occurrence_available[channel_index] = true;
            result.bins[bin_index].occurrences[channel_index] =
                object->histogram.bins[bin_index].count;
        }
    }
    return result;
}

vector<AggregatedPortResult> aggregate_results(
    const vector<MappingEntry>& selected,
    const vector<ObjectResult>& objects,
    const vector<ModuleResult>& modules,
    bool counters_requested,
    bool histogram_requested,
    bool module_requested)
{
    map<string, map<unsigned, vector<MappingEntry> > > grouped;
    for (const MappingEntry& entry : selected)
        grouped[entry.label_port][entry.ipil].push_back(entry);

    vector<AggregatedPortResult> results;
    for (auto& port_item : grouped)
    {
        AggregatedPortResult port;
        port.label_port = port_item.first;
        port.module_requested = module_requested;
        if (module_requested)
        {
            const ModuleResult* module = find_module(modules, port.label_port);
            if (module)
            {
                port.module_metadata = *module;
                port.module_snapshot = module->snapshot;
                port.module_result_available = true;
            }
        }

        for (auto& ipil_item : port_item.second)
        {
            vector<MappingEntry>& entries = ipil_item.second;
            sort(entries.begin(), entries.end(), [](const MappingEntry& a,
                                                     const MappingEntry& b) {
                return a.channel < b.channel;
            });

            AggregatedLogicalPort logical;
            logical.ipil = ipil_item.first;
            logical.name = port.label_port + "/" + to_string(logical.ipil);
            logical.counters_requested = counters_requested;
            logical.histogram_requested = histogram_requested;
            for (const MappingEntry& entry : entries)
            {
                logical.channels.push_back(entry.channel);
                if (counters_requested)
                {
                    AggregatedCounterSlot slot;
                    slot.channel = entry.channel;
                    const ObjectResult* object =
                        find_object(objects, entry.label_port, entry.channel);
                    if (object && object->counters_success)
                    {
                        slot.available = true;
                        slot.value = object->ppcnt;
                    }
                    logical.counters.push_back(slot);
                }
            }

            if (histogram_requested)
                logical.histogram = aggregate_histogram(entries, objects);

            if (port.module_result_available && port.module_metadata.success &&
                port.module_snapshot.present)
            {
                const unsigned first_lane = (logical.ipil - 1) * 4;
                const unsigned end_lane = first_lane + 4;
                for (const q3400::ModuleMediaLane& lane :
                     port.module_snapshot.media_lanes)
                {
                    if (lane.lane >= first_lane && lane.lane < end_lane)
                        logical.module_lanes.push_back(lane);
                }
            }
            port.logical_ports.push_back(logical);
        }
        results.push_back(port);
    }
    sort(results.begin(), results.end(), [](const AggregatedPortResult& a,
                                             const AggregatedPortResult& b) {
        return port_number(a.label_port) < port_number(b.label_port);
    });
    return results;
}

void require_self_test(bool condition, const string& message)
{
    if (!condition)
        throw runtime_error("aggregation self-test failed: " + message);
}

void run_aggregation_self_test()
{
    vector<MappingEntry> selected;
    vector<ObjectResult> objects;
    for (unsigned channel = 1; channel <= 8; ++channel)
    {
        MappingEntry entry;
        entry.label_port = "1";
        entry.ipil = channel <= 4 ? 1 : 2;
        entry.channel = channel;
        entry.mapping_channel = ((channel - 1) % 4) + 1;
        selected.push_back(entry);

        ObjectResult object;
        object.topology = entry;
        object.success = true;
        object.counters_requested = true;
        object.counters_success = true;
        object.histogram_requested = true;
        object.histogram_success = true;
        object.ppcnt.raw_error = 100 + channel;
        object.ppcnt.symbol_errors = 200 + channel;
        object.ppcnt.raw_ber_coefficient = channel;
        object.ppcnt.raw_ber_magnitude = 12;
        object.histogram.active_type = 3;
        HistogramBin first;
        first.index = 0;
        first.low = 0;
        first.high = 10;
        first.count = 1000 + channel;
        object.histogram.bins.push_back(first);
        HistogramBin second;
        second.index = 1;
        second.low = 11;
        second.high = 20;
        second.count = 2000 + channel;
        object.histogram.bins.push_back(second);
        objects.push_back(object);
    }

    ModuleResult module;
    module.label_port = "1";
    module.success = true;
    module.snapshot.present = true;
    module.snapshot.vendor = "TEST_VENDOR";
    module.snapshot.part_number = "TEST_PN";
    module.snapshot.serial_number = "TEST_SN";
    module.snapshot.revision = "R1";
    module.snapshot.memory_map_revision = 5;
    module.snapshot.memory_map_revision_display = "5";
    module.snapshot.memory_map_compliance_display = "CMIS";
    module.snapshot.cable_identifier = 8;
    module.snapshot.cable_identifier_name = "OSFP";
    module.snapshot.cable_type = 2;
    module.snapshot.cable_type_name = "Optical Module (separated)";
    module.snapshot.cable_technology_name = "850 nm VCSEL";
    module.snapshot.wavelength_available = true;
    module.snapshot.wavelength_nm = 850;
    module.snapshot.module_state_available = true;
    module.snapshot.module_state = 3;
    module.snapshot.module_state_name =
        q3400::PddrModuleSnapshotReader::format_module_state(3);
    module.snapshot.fw_version_available = true;
    module.snapshot.fw_version = 0x01000000;
    module.snapshot.fw_version_string =
        q3400::PddrModuleSnapshotReader::format_fw_version(module.snapshot.fw_version);
    module.snapshot.temperature_available = true;
    module.snapshot.temperature_c = 42.5;
    module.snapshot.temperature_thresholds_available = true;
    module.snapshot.temperature_low_c = -5.0;
    module.snapshot.temperature_high_c = 85.0;
    module.snapshot.voltage_available = true;
    module.snapshot.voltage_mv = 3290.5;
    module.snapshot.voltage_thresholds_available = true;
    module.snapshot.voltage_low_mv = 3000.0;
    module.snapshot.voltage_high_mv = 3600.0;
    module.snapshot.rx_power_thresholds_available = true;
    module.snapshot.rx_power_low_uw = 10;
    module.snapshot.rx_power_high_uw = 2000;
    module.snapshot.tx_power_thresholds_available = true;
    module.snapshot.tx_power_low_uw = 20;
    module.snapshot.tx_power_high_uw = 1800;
    module.snapshot.tx_bias_thresholds_available = true;
    module.snapshot.tx_bias_low_ua = 1000.0;
    module.snapshot.tx_bias_high_ua = 12000.0;
    module.snapshot.optional_fields.insert("monitor_cap_mask");
    module.snapshot.optional_fields.insert("rx_output_valid_cap");
    module.snapshot.optional_fields.insert("rx_output_valid");
    for (unsigned lane_number = 0; lane_number < 8; ++lane_number)
    {
        q3400::ModuleMediaLane lane;
        lane.lane = lane_number;
        lane.rx_power_available = true;
        lane.rx_power_uw = 3000 + lane_number;
        lane.datapath_state_available = true;
        lane.datapath_state = 4;
        lane.datapath_state_name =
            q3400::PddrModuleSnapshotReader::format_datapath_state(4);
        lane.rx_output_valid_available = true;
        lane.rx_output_valid_supported = false;
        lane.rx_output_valid = false;
        module.snapshot.media_lanes.push_back(lane);
    }

    const vector<AggregatedPortResult> all =
        aggregate_results(selected, objects, vector<ModuleResult>(1, module),
                          true, true, true);
    require_self_test(all.size() == 1, "one physical port");
    require_self_test(all[0].module_result_available,
                      "one shared physical module result");
    require_self_test(all[0].module_snapshot.temperature_available &&
                      all[0].module_snapshot.temperature_c == 42.5 &&
                      all[0].module_snapshot.voltage_available &&
                      all[0].module_snapshot.voltage_mv == 3290.5,
                      "module identity/status/monitors copied from typed snapshot");
    require_self_test(all[0].module_snapshot.fw_version_string == "1.0.0" &&
                      all[0].module_snapshot.module_state_name == "Ready state" &&
                      all[0].module_snapshot.temperature_low_c == -5.0 &&
                      all[0].module_snapshot.rx_power_high_uw == 2000,
                      "PDDR3 readable fields and thresholds materialized");
    require_self_test(all[0].module_snapshot.media_lanes[0].rx_output_valid_available &&
                      !all[0].module_snapshot.media_lanes[0].rx_output_valid &&
                      all[0].module_snapshot.media_lanes[0].datapath_state_name ==
                          "DPActivated",
                      "false Rx Output Valid remains materialized per lane");
    require_self_test(all[0].logical_ports.size() == 2, "two logical ports");
    const AggregatedLogicalPort& first = all[0].logical_ports[0];
    const AggregatedLogicalPort& second = all[0].logical_ports[1];
    require_self_test(first.name == "1/1" && first.channels == vector<unsigned>({1,2,3,4}),
                      "CH1-CH4 map to 1/1");
    require_self_test(second.name == "1/2" && second.channels == vector<unsigned>({5,6,7,8}),
                      "CH5-CH8 map to 1/2");
    require_self_test(first.module_lanes.size() == 4 &&
                      first.module_lanes.front().lane == 0 &&
                      first.module_lanes.back().lane == 3,
                      "media lanes 0-3 map to 1/1");
    require_self_test(second.module_lanes.size() == 4 &&
                      second.module_lanes.front().lane == 4 &&
                      second.module_lanes.back().lane == 7,
                      "media lanes 4-7 map to 1/2");
    require_self_test(first.counters.size() == 4 &&
                      first.counters[0].value.raw_error == 101 &&
                      first.counters[3].value.raw_error == 104,
                      "four channel counter values remain distinct");
    require_self_test(first.histogram.bins.size() == 2 &&
                      first.histogram.bins[0].low == 0 &&
                      first.histogram.bins[0].high == 10 &&
                      first.histogram.bins[0].occurrences ==
                          vector<uint64_t>({1001,1002,1003,1004}),
                      "one range with four occurrence values");

    q3400::ModuleSnapshotData missing_optional;
    require_self_test(!missing_optional.has_optional("date_code") &&
                      !missing_optional.has_optional("vendor_oui"),
                      "missing manufacturing date and another optional field remain unavailable");
    const q3400::PddrSchemaError schema_error("fixture schema mismatch");
    const runtime_error access_error("fixture device access failure");
    require_self_test(!q3400::module_context_fallback_allowed(schema_error),
                      "deterministic schema failure does not trigger context fallback");
    require_self_test(q3400::module_context_fallback_allowed(access_error),
                      "access/context failure remains eligible for fallback");

    vector<MappingEntry> first_half(selected.begin(), selected.begin() + 4);
    vector<ObjectResult> first_half_objects(objects.begin(), objects.begin() + 4);
    const vector<AggregatedPortResult> partial =
        aggregate_results(first_half, first_half_objects,
                          vector<ModuleResult>(1, module), true, true, true);
    require_self_test(partial.size() == 1 &&
                      partial[0].logical_ports.size() == 1 &&
                      partial[0].logical_ports[0].name == "1/1",
                      "partial selection does not create empty 1/2");

    module.snapshot.media_lanes.resize(5);
    const vector<AggregatedPortResult> short_module =
        aggregate_results(selected, objects, vector<ModuleResult>(1, module),
                          true, true, true);
    require_self_test(short_module[0].logical_ports[1].module_lanes.size() == 1 &&
                      short_module[0].logical_ports[1].module_lanes[0].lane == 4,
                      "short media-lane vector is sliced without padding");

    ScanResult synthetic;
    synthetic.objects = objects;
    synthetic.modules.push_back(module);
    synthetic.aggregated_ports = all;
    synthetic.success_count = objects.size();
    synthetic.module_success_count = 1;
    print_scan_json(synthetic);
}

string utc_timestamp()
{
    const time_t now = time(nullptr);
    struct tm value;
    if (!gmtime_r(&now, &value))
        return "";
    char buffer[32];
    if (strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &value) == 0)
        return "";
    return buffer;
}

uint64_t combine_u32(uint64_t high, uint64_t low)
{
    return (high << 32) | low;
}

class DeviceContext
{
public:
    DeviceContext(const string& bdf,
                  const string& adb_file,
                  const vector<MappingEntry>& entries,
                  bool read_counters,
                  bool read_histogram,
                  bool read_modules) :
        _bdf(bdf), _mf(nullptr)
    {
        _mf = mopen(_bdf.c_str());
        if (!_mf)
            throw runtime_error("mopen failed for " + _bdf);
        try
        {
            _reg.reset(new MlxRegLib(_mf, adb_file, true));
            _node = (read_counters || read_histogram) ?
                _reg->findAdbNode("PPCNT") : nullptr;
            _pphcr_node = read_histogram ? _reg->findAdbNode("PPHCR") : nullptr;
            if ((read_counters && !_node) || (read_histogram && (!_node || !_pphcr_node)))
                throw runtime_error("required PPCNT/PPHCR node not found in ADB for " + _bdf);
            uint32_t bytes = 0;
            if (read_counters || read_histogram)
            {
                bytes = static_cast<uint32_t>(_node->get_size() / 8);
                if (bytes == 0 || bytes % sizeof(u_int32_t) != 0)
                    throw runtime_error("invalid PPCNT layout size for " + _bdf);
                _expected_dwords = bytes / sizeof(u_int32_t);
            }
            if (read_modules)
                _module_reader.reset(new q3400::PddrModuleSnapshotReader(*_reg));
            uint32_t pphcr_bytes = 0;
            if (read_histogram)
            {
                pphcr_bytes = static_cast<uint32_t>(_pphcr_node->get_size() / 8);
                if (pphcr_bytes != 92 || pphcr_bytes % sizeof(u_int32_t) != 0)
                    throw runtime_error("invalid PPHCR layout size for " + _bdf);
                _pphcr_expected_dwords = pphcr_bytes / sizeof(u_int32_t);
            }

            set<unsigned> local_ports;
            for (const MappingEntry& entry : entries)
                local_ports.insert(entry.local_port);
            for (unsigned local_port : local_ports)
            {
                if (read_counters)
                {
                    const string indexes = "grp=0x16,local_port=" + to_string(local_port) +
                                           ",lp_msb=0,pnat=0,plane_ind=0";
                    RegAccessParser parser("", indexes, "clr=0", &_reg->getAdb(),
                                           _node, bytes, false, false);
                    vector<u_int32_t> request = parser.genBuff();
                    if (request.size() != _expected_dwords)
                        throw runtime_error("request size mismatch for " + describe(local_port));
                    if (_fields.empty())
                    {
                        capture_fields(request, _fields);
                        require_payload_fields(_fields);
                    }
                    validate_header(request, _fields, local_port, "request template");
                    _templates[local_port] = request;
                }

                if (read_histogram)
                {
                    const string pphcr_indexes =
                        "local_port=" + to_string(local_port) +
                        ",lp_msb=0,pnat=0,plane_ind=0,port_type=0,hist_type=0";
                    RegAccessParser pphcr_parser("", pphcr_indexes, "", &_reg->getAdb(),
                                                 _pphcr_node, pphcr_bytes, false, false);
                    vector<u_int32_t> pphcr_request = pphcr_parser.genBuff();
                    if (_pphcr_fields.empty())
                        capture_all_fields(_pphcr_node, pphcr_request, _pphcr_fields);
                    validate_pphcr(pphcr_request, local_port, "request template");
                    _pphcr_templates[local_port] = pphcr_request;

                    const string histogram_indexes =
                        "grp=0x23,local_port=" + to_string(local_port) +
                        ",lp_msb=0,pnat=0,plane_ind=0,port_type=0,lp_gl=1";
                    RegAccessParser histogram_parser("", histogram_indexes, "clr=0",
                                                      &_reg->getAdb(), _node, bytes,
                                                      false, false);
                    vector<u_int32_t> histogram_request = histogram_parser.genBuff();
                    if (_histogram_fields.empty())
                        capture_all_fields(_node, histogram_request, _histogram_fields);
                    validate_histogram_header(histogram_request, local_port, "request template");
                    _histogram_templates[local_port] = histogram_request;
                }
                if (read_modules)
                    _module_templates[local_port] =
                        _module_reader->build_request(local_port);
            }
        }
        catch (...)
        {
            close();
            throw;
        }
    }

    ~DeviceContext() { close(); }

    PpcntValue read(unsigned local_port)
    {
        map<unsigned, vector<u_int32_t> >::const_iterator request =
            _templates.find(local_port);
        if (request == _templates.end())
            throw runtime_error("no request template for " + describe(local_port));
        vector<u_int32_t> response = request->second;
        _reg->sendRegister("PPCNT", MACCESS_REG_METHOD_GET, response);

        validate_header(response, _fields, local_port, "response");

        PpcntValue value;
        value.time_since_last_clear_ms =
            combine_u32(pop(_fields, "time_since_last_clear_high", response),
                        pop(_fields, "time_since_last_clear_low", response));
        value.received_bits = combine_u32(pop(_fields, "phy_received_bits_high", response),
                                          pop(_fields, "phy_received_bits_low", response));
        value.symbol_errors = combine_u32(pop(_fields, "phy_symbol_errors_high", response),
                                          pop(_fields, "phy_symbol_errors_low", response));
        value.corrected_bits = combine_u32(pop(_fields, "phy_corrected_bits_high", response),
                                           pop(_fields, "phy_corrected_bits_low", response));
        value.raw_error = combine_u32(pop(_fields, "phy_raw_errors_lane0_high", response),
                                      pop(_fields, "phy_raw_errors_lane0_low", response));
        value.effective_errors = combine_u32(pop(_fields, "phy_effective_errors_high", response),
                                             pop(_fields, "phy_effective_errors_low", response));
        value.raw_ber_coefficient = static_cast<uint32_t>(pop(_fields, "raw_ber_coef", response));
        value.raw_ber_magnitude = static_cast<uint32_t>(pop(_fields, "raw_ber_magnitude", response));
        value.raw_ber_lane0_coefficient =
            static_cast<uint32_t>(pop(_fields, "raw_ber_coef_lane0", response));
        value.raw_ber_lane0_magnitude =
            static_cast<uint32_t>(pop(_fields, "raw_ber_magnitude_lane0", response));
        value.effective_ber_coefficient =
            static_cast<uint32_t>(pop(_fields, "effective_ber_coef", response));
        value.effective_ber_magnitude =
            static_cast<uint32_t>(pop(_fields, "effective_ber_magnitude", response));
        value.symbol_ber_coefficient =
            static_cast<uint32_t>(pop(_fields, "symbol_ber_coef", response));
        value.symbol_ber_magnitude =
            static_cast<uint32_t>(pop(_fields, "symbol_ber_magnitude", response));
        return value;
    }

    HistogramValue read_histogram(unsigned local_port)
    {
        map<unsigned, vector<u_int32_t> >::const_iterator pphcr_template =
            _pphcr_templates.find(local_port);
        map<unsigned, vector<u_int32_t> >::const_iterator histogram_template =
            _histogram_templates.find(local_port);
        if (pphcr_template == _pphcr_templates.end() ||
            histogram_template == _histogram_templates.end())
            throw runtime_error("no histogram request template for " + describe(local_port));

        vector<u_int32_t> pphcr = pphcr_template->second;
        _reg->sendRegister("PPHCR", MACCESS_REG_METHOD_GET, pphcr);
        validate_pphcr(pphcr, local_port, "response");

        HistogramValue value;
        value.active_type = static_cast<uint32_t>(pop_path(_pphcr_fields,
                                                            {"active_hist_type"},
                                                            pphcr));
        const unsigned num_bins = static_cast<unsigned>(
            pop_path(_pphcr_fields, {"num_of_bins"}, pphcr));
        if (value.active_type == 0)
            throw ObjectReadError("no active FEC histogram for " + describe(local_port));
        if (num_bins == 0 || num_bins > MAX_HISTOGRAM_BINS)
            throw ObjectReadError("invalid histogram bin count for " + describe(local_port));

        vector<u_int32_t> counters = histogram_template->second;
        _reg->sendRegister("PPCNT", MACCESS_REG_METHOD_GET, counters);
        validate_histogram_header(counters, local_port, "response");

        for (unsigned index = 0; index < num_bins; ++index)
        {
            const string number = to_string(index);
            HistogramBin bin;
            bin.index = index;
            bin.low = static_cast<uint32_t>(pop_path(
                _pphcr_fields,
                {"bin_range[" + number + "].low_val", "low_val_" + number,
                 "bin_range_" + number + ".low_val"}, pphcr));
            bin.high = static_cast<uint32_t>(pop_path(
                _pphcr_fields,
                {"bin_range[" + number + "].high_val", "high_val_" + number,
                 "bin_range_" + number + ".high_val"}, pphcr));
            const uint64_t high = pop_path(
                _histogram_fields,
                {"hist[" + number + "].hi", "hist[" + number + "]_hi",
                 "hist_" + number + ".hi"}, counters);
            const uint64_t low = pop_path(
                _histogram_fields,
                {"hist[" + number + "].lo", "hist[" + number + "]_lo",
                 "hist_" + number + ".lo"}, counters);
            bin.count = combine_u32(high, low);
            value.bins.push_back(bin);
        }
        return value;
    }

    q3400::ModuleSnapshotData read_module(unsigned local_port)
    {
        map<unsigned, vector<u_int32_t> >::const_iterator request =
            _module_templates.find(local_port);
        if (!_module_reader || request == _module_templates.end())
            throw runtime_error("no PDDR page 3 request template for " + describe(local_port));
        return _module_reader->read(local_port, request->second);
    }

private:
    typedef MlxRegLib::AdbInstance AdbInstance;
    typedef map<string, AdbInstance*> FieldSet;

    struct AllCaptureContext
    {
        FieldSet* fields;
    };

    struct CaptureContext
    {
        FieldSet* fields;
    };

    static bool capture(const string&,
                        uint64_t,
                        uint64_t,
                        AdbInstance* instance,
                        void* opaque)
    {
        if (!instance || !instance->fieldDesc)
            return false;
        static const set<string> wanted = {
            "grp", "local_port", "clr",
            "time_since_last_clear_high", "time_since_last_clear_low",
            "phy_received_bits_high", "phy_received_bits_low",
            "phy_symbol_errors_high", "phy_symbol_errors_low",
            "phy_corrected_bits_high", "phy_corrected_bits_low",
            "phy_raw_errors_lane0_high", "phy_raw_errors_lane0_low",
            "phy_effective_errors_high", "phy_effective_errors_low",
            "raw_ber_coef", "raw_ber_magnitude",
            "raw_ber_coef_lane0", "raw_ber_magnitude_lane0",
            "effective_ber_coef", "effective_ber_magnitude",
            "symbol_ber_coef", "symbol_ber_magnitude"
        };
        const string& name = instance->fieldDesc->name;
        if (wanted.count(name) != 0)
            static_cast<CaptureContext*>(opaque)->fields->insert(make_pair(name, instance));
        return false;
    }

    static bool capture_all(const string& path,
                            uint64_t,
                            uint64_t,
                            AdbInstance* instance,
                            void* opaque)
    {
        if (!instance || !instance->fieldDesc)
            return false;
        FieldSet& fields = *static_cast<AllCaptureContext*>(opaque)->fields;
        fields.insert(make_pair(instance->fieldDesc->name, instance));
        fields.insert(make_pair(path, instance));
        return false;
    }

    void capture_fields(const vector<u_int32_t>& buffer, FieldSet& fields)
    {
        CaptureContext context = {&fields};
        _reg->getAdb().traverse_layout(
            _node, "", 0, reinterpret_cast<const uint8_t*>(&buffer[0]),
            static_cast<uint32_t>(buffer.size() * sizeof(u_int32_t)),
            capture, &context, true, false, true);
    }

    void capture_all_fields(AdbInstance* node,
                            const vector<u_int32_t>& buffer,
                            FieldSet& fields)
    {
        AllCaptureContext context = {&fields};
        _reg->getAdb().traverse_layout(
            node, "", 0, reinterpret_cast<const uint8_t*>(&buffer[0]),
            static_cast<uint32_t>(buffer.size() * sizeof(u_int32_t)),
            capture_all, &context, true, false, true);
    }

    static uint64_t pop(const FieldSet& fields,
                        const string& name,
                        const vector<u_int32_t>& buffer)
    {
        FieldSet::const_iterator field = fields.find(name);
        if (field == fields.end())
            throw runtime_error("PPCNT layout is missing field: " + name);
        return field->second->popBuf(reinterpret_cast<uint8_t*>(
            const_cast<u_int32_t*>(&buffer[0])));
    }

    static uint64_t pop_path(const FieldSet& fields,
                             const vector<string>& candidates,
                             const vector<u_int32_t>& buffer)
    {
        for (const string& candidate : candidates)
        {
            FieldSet::const_iterator exact = fields.find(candidate);
            if (exact != fields.end())
                return exact->second->popBuf(reinterpret_cast<uint8_t*>(
                    const_cast<u_int32_t*>(&buffer[0])));
            const string suffix = "." + candidate;
            for (FieldSet::const_iterator it = fields.begin(); it != fields.end(); ++it)
            {
                if (it->first.size() >= suffix.size() &&
                    it->first.compare(it->first.size() - suffix.size(), suffix.size(), suffix) == 0)
                    return it->second->popBuf(reinterpret_cast<uint8_t*>(
                        const_cast<u_int32_t*>(&buffer[0])));
            }
        }
        throw runtime_error("layout is missing field: " + candidates.front());
    }

    void validate_header(const vector<u_int32_t>& buffer,
                         const FieldSet& fields,
                         unsigned local_port,
                         const string& phase) const
    {
        if (buffer.size() != _expected_dwords)
            throw runtime_error(phase + " size mismatch for " + describe(local_port));
        if (pop(fields, "grp", buffer) != PPCNT_GROUP)
            throw runtime_error(phase + " grp mismatch for " + describe(local_port));
        if (pop(fields, "local_port", buffer) != local_port)
            throw runtime_error(phase + " local_port mismatch for " + describe(local_port));
        if (pop(fields, "clr", buffer) != 0)
            throw runtime_error(phase + " has unsafe clr != 0 for " + describe(local_port));
    }

    void validate_pphcr(const vector<u_int32_t>& buffer,
                         unsigned local_port,
                         const string& phase) const
    {
        if (buffer.size() != _pphcr_expected_dwords)
            throw runtime_error(phase + " PPHCR size mismatch for " + describe(local_port));
        if (pop_path(_pphcr_fields, {"local_port"}, buffer) != local_port ||
            pop_path(_pphcr_fields, {"hist_type"}, buffer) != 0)
            throw runtime_error(phase + " PPHCR index mismatch for " + describe(local_port));
    }

    void validate_histogram_header(const vector<u_int32_t>& buffer,
                                   unsigned local_port,
                                   const string& phase) const
    {
        if (buffer.size() != _expected_dwords)
            throw runtime_error(phase + " histogram size mismatch for " + describe(local_port));
        if (pop_path(_histogram_fields, {"grp"}, buffer) != HISTOGRAM_GROUP ||
            pop_path(_histogram_fields, {"local_port"}, buffer) != local_port)
            throw runtime_error(phase + " histogram index mismatch for " + describe(local_port));
        if (pop_path(_histogram_fields, {"clr"}, buffer) != 0)
            throw runtime_error(phase + " histogram has unsafe clr != 0 for " +
                                describe(local_port));
    }

    static void require_payload_fields(const FieldSet& fields)
    {
        static const char* required[] = {
            "time_since_last_clear_high", "time_since_last_clear_low",
            "phy_received_bits_high", "phy_received_bits_low",
            "phy_symbol_errors_high", "phy_symbol_errors_low",
            "phy_corrected_bits_high", "phy_corrected_bits_low",
            "phy_raw_errors_lane0_high", "phy_raw_errors_lane0_low",
            "phy_effective_errors_high", "phy_effective_errors_low",
            "raw_ber_coef", "raw_ber_magnitude",
            "raw_ber_coef_lane0", "raw_ber_magnitude_lane0",
            "effective_ber_coef", "effective_ber_magnitude",
            "symbol_ber_coef", "symbol_ber_magnitude"
        };
        for (const char* name : required)
        {
            if (fields.count(name) == 0)
                throw runtime_error("PPCNT group 0x16 layout is missing field: " + string(name));
        }
    }

    string describe(unsigned local_port) const
    {
        return _bdf + "/local_port " + to_string(local_port);
    }

    void close()
    {
        _module_reader.reset();
        _reg.reset();
        if (_mf)
        {
            mclose(_mf);
            _mf = nullptr;
        }
    }

    string _bdf;
    mfile* _mf;
    unique_ptr<MlxRegLib> _reg;
    unique_ptr<q3400::PddrModuleSnapshotReader> _module_reader;
    AdbInstance* _node = nullptr;
    AdbInstance* _pphcr_node = nullptr;
    size_t _expected_dwords = 0;
    size_t _pphcr_expected_dwords = 0;
    FieldSet _fields;
    FieldSet _pphcr_fields;
    FieldSet _histogram_fields;
    map<unsigned, vector<u_int32_t> > _templates;
    map<unsigned, vector<u_int32_t> > _pphcr_templates;
    map<unsigned, vector<u_int32_t> > _histogram_templates;
    map<unsigned, vector<u_int32_t> > _module_templates;
};

class Reader
{
public:
    Reader(const string& adb_file,
           const vector<MappingEntry>& selected,
           const vector<ModulePlan>& module_plans,
           bool read_counters,
           bool read_histogram,
           bool read_modules) :
        _selected(selected),
        _module_plans(module_plans),
        _read_counters(read_counters),
        _read_histogram(read_histogram),
        _read_modules(read_modules)
    {
        require_readable_file(adb_file, "ADB");
        map<string, vector<MappingEntry> > execution_buckets;
        if (read_counters || read_histogram)
        {
            for (const MappingEntry& entry : selected)
            {
                _buckets[entry.bdf].push_back(entry);
                execution_buckets[entry.bdf].push_back(entry);
            }
        }
        if (read_modules)
        {
            for (const ModulePlan& plan : _module_plans)
            {
                for (const MappingEntry& entry : plan.contexts)
                    execution_buckets[entry.bdf].push_back(entry);
            }
        }
        if (execution_buckets.empty() || execution_buckets.size() > 4)
            throw invalid_argument("selection must use between one and four device contexts");
        for (const auto& bucket : execution_buckets)
        {
            _contexts[bucket.first].reset(new DeviceContext(
                bucket.first, adb_file, bucket.second,
                read_counters, read_histogram, read_modules));
        }
    }

    ScanResult read_once(double cold_start_ms)
    {
        ScanResult result;
        result.cold_start_ms = cold_start_ms;
        vector<string> devices;
        vector<vector<ObjectResult> > worker_results(_buckets.size());
        vector<thread> workers;
        const Clock::time_point scan_start = Clock::now();
        const Clock::time_point channel_start = scan_start;

        for (const auto& bucket : _buckets)
            devices.push_back(bucket.first);
        for (size_t worker_index = 0; worker_index < devices.size(); ++worker_index)
        {
            const string bdf = devices[worker_index];
            workers.push_back(thread([this, bdf, worker_index, &worker_results]() {
                vector<ObjectResult> local_results;
                string fatal;
                const vector<MappingEntry>& entries = _buckets.at(bdf);
                for (size_t i = 0; i < entries.size(); ++i)
                {
                    ObjectResult object;
                    object.topology = entries[i];
                    object.counters_requested = _read_counters;
                    object.histogram_requested = _read_histogram;
                    if (!fatal.empty())
                    {
                        object.error = "device worker stopped after error: " + fatal;
                    }
                    else
                    {
                        try
                        {
                            if (_read_counters)
                            {
                                object.ppcnt = _contexts.at(bdf)->read(entries[i].local_port);
                                object.counters_success = true;
                            }
                            if (_read_histogram)
                            {
                                object.histogram =
                                    _contexts.at(bdf)->read_histogram(entries[i].local_port);
                                object.histogram_success = true;
                            }
                            object.success = true;
                        }
                        catch (const ObjectReadError& error)
                        {
                            object.error = error.what();
                        }
                        catch (const exception& error)
                        {
                            object.error = error.what();
                            fatal = object.error;
                        }
                    }
                    local_results.push_back(object);
                }
                worker_results[worker_index].swap(local_results);
            }));
        }
        for (thread& worker : workers)
            worker.join();
        const Clock::time_point channel_end = Clock::now();

        for (const vector<ObjectResult>& objects : worker_results)
        {
            result.objects.insert(result.objects.end(), objects.begin(), objects.end());
        }
        sort(result.objects.begin(), result.objects.end(), [](const ObjectResult& a, const ObjectResult& b) {
            return a.topology.source_line < b.topology.source_line;
        });
        for (const ObjectResult& object : result.objects)
        {
            if (object.success)
                ++result.success_count;
            else
                ++result.failed_count;
        }

        const Clock::time_point module_start = Clock::now();
        if (_read_modules)
        {
            for (const ModulePlan& plan : _module_plans)
            {
                ModuleResult module;
                module.label_port = plan.label_port;
                module.canonical_context = plan.contexts.front();
                vector<string> errors;
                bool schema_failure = false;
                for (size_t attempt = 0; attempt < plan.contexts.size(); ++attempt)
                {
                    const MappingEntry& context = plan.contexts[attempt];
                    try
                    {
                        module.snapshot =
                            _contexts.at(context.bdf)->read_module(context.local_port);
                        module.actual_context = context;
                        module.fallback_used = attempt != 0;
                        module.success = true;
                        module.updated_at = utc_timestamp();
                        break;
                    }
                    catch (const exception& error)
                    {
                        errors.push_back(context.pciconf + "/local_port " +
                                         to_string(context.local_port) + ": " + error.what());
                        if (!q3400::module_context_fallback_allowed(error))
                        {
                            schema_failure = true;
                            break;
                        }
                    }
                }
                if (!module.success)
                {
                    ostringstream message;
                    if (schema_failure)
                        message << "PDDR page 3 deterministic schema failure";
                    else
                        message << "all " << plan.contexts.size()
                                << " PDDR page 3 contexts failed";
                    for (const string& error : errors)
                        message << "; " << error;
                    module.error = message.str();
                    ++result.module_failed_count;
                }
                else
                {
                    ++result.module_success_count;
                    if (!module.snapshot.present)
                        ++result.module_absent_count;
                }
                result.modules.push_back(module);
            }
        }
        const Clock::time_point module_end = Clock::now();
        const Clock::time_point scan_end = module_end;

        result.channel_scan_ms =
            chrono::duration<double, milli>(channel_end - channel_start).count();
        result.module_scan_ms =
            chrono::duration<double, milli>(module_end - module_start).count();
        result.scan_ms = chrono::duration<double, milli>(scan_end - scan_start).count();
        result.total_ms = result.cold_start_ms + result.scan_ms;
        result.aggregated_ports = aggregate_results(
            _selected, result.objects, result.modules,
            _read_counters, _read_histogram, _read_modules);
        return result;
    }

private:
    vector<MappingEntry> _selected;
    map<string, vector<MappingEntry> > _buckets;
    map<string, unique_ptr<DeviceContext> > _contexts;
    vector<ModulePlan> _module_plans;
    bool _read_counters;
    bool _read_histogram;
    bool _read_modules;
};

string json_escape(const string& value)
{
    ostringstream output;
    for (unsigned char c : value)
    {
        switch (c)
        {
            case '\\': output << "\\\\"; break;
            case '"': output << "\\\""; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (c < 0x20)
                    output << "\\u" << hex << setw(4) << setfill('0') << unsigned(c) << dec;
                else
                    output << c;
        }
    }
    return output.str();
}

void print_context_json(const MappingEntry& context)
{
    cout << "{\"pciconf\":\"" << context.pciconf
         << "\",\"bdf\":\"" << context.bdf
         << "\",\"local_port\":" << context.local_port
         << ",\"ipil\":" << context.ipil
         << ",\"logical_channel\":" << context.channel
         << ",\"module\":\"" << json_escape(context.module)
         << "\",\"sub_module\":\"" << json_escape(context.sub_module) << "\"}";
}

void print_media_lane_json(const q3400::ModuleMediaLane& lane)
{
    cout << "{\"lane\":" << lane.lane << ",\"rx_power_uw\":";
    if (lane.rx_power_available) cout << lane.rx_power_uw; else cout << "null";
    cout << ",\"tx_power_uw\":";
    if (lane.tx_power_available) cout << lane.tx_power_uw; else cout << "null";
    cout << ",\"tx_bias_ua\":";
    if (lane.tx_bias_available) cout << lane.tx_bias_ua; else cout << "null";
    cout << ",\"datapath_state\":";
    if (lane.datapath_state_available) cout << lane.datapath_state; else cout << "null";
    cout << ",\"datapath_state_name\":";
    if (lane.datapath_state_available)
        cout << "\"" << json_escape(lane.datapath_state_name) << "\"";
    else
        cout << "null";
    cout << ",\"rx_output_valid\":";
    if (lane.rx_output_valid_available)
        cout << (lane.rx_output_valid ? "true" : "false");
    else
        cout << "null";
    cout << ",\"rx_output_valid_capable\":"
         << (lane.rx_output_valid_supported ? "true" : "false");
    cout << "}";
}

void print_module_json(const ModuleResult& module)
{
    cout << "{\"label_port\":\"" << json_escape(module.label_port)
         << "\",\"success\":" << (module.success ? "true" : "false")
         << ",\"canonical_context\":";
    print_context_json(module.canonical_context);
    cout << ",\"fallback_used\":" << (module.fallback_used ? "true" : "false");
    if (module.success)
    {
        const q3400::ModuleSnapshotData& snapshot = module.snapshot;
        cout << ",\"actual_context\":";
        print_context_json(module.actual_context);
        cout << ",\"present\":" << (snapshot.present ? "true" : "false")
             << ",\"identity\":{\"vendor\":";
        if (snapshot.present) cout << "\"" << json_escape(snapshot.vendor) << "\"";
        else cout << "null";
        cout << ",\"part_number\":";
        if (snapshot.present) cout << "\"" << json_escape(snapshot.part_number) << "\"";
        else cout << "null";
        cout << ",\"serial_number\":";
        if (snapshot.present) cout << "\"" << json_escape(snapshot.serial_number) << "\"";
        else cout << "null";
        cout << ",\"revision\":";
        if (snapshot.present) cout << "\"" << json_escape(snapshot.revision) << "\"";
        else cout << "null";
        cout << ",\"memory_map_revision\":";
        if (snapshot.present) cout << snapshot.memory_map_revision; else cout << "null";
        cout << ",\"memory_map_revision_display\":";
        if (snapshot.present)
            cout << "\"" << json_escape(snapshot.memory_map_revision_display) << "\"";
        else cout << "null";
        cout << ",\"memory_map_compliance_raw\":";
        if (snapshot.present && snapshot.has_optional("memory_map_compliance"))
            cout << snapshot.memory_map_compliance;
        else cout << "null";
        cout << ",\"memory_map_compliance\":";
        if (snapshot.present && snapshot.has_optional("memory_map_compliance") &&
            !snapshot.memory_map_compliance_display.empty())
            cout << "\"" << json_escape(snapshot.memory_map_compliance_display) << "\"";
        else cout << "null";
        cout << ",\"vendor_oui_raw\":";
        if (snapshot.present && snapshot.has_optional("vendor_oui"))
            cout << snapshot.vendor_oui;
        else cout << "null";
        cout << ",\"vendor_oui\":";
        if (snapshot.present && snapshot.has_optional("vendor_oui"))
            cout << "\"" << snapshot.vendor_oui_hex << "\"";
        else cout << "null";
        cout << ",\"manufacturing_date_raw\":";
        if (snapshot.present && snapshot.has_optional("date_code"))
            cout << snapshot.manufacturing_date_raw;
        else cout << "null";
        cout << ",\"manufacturing_date\":";
        if (snapshot.present && !snapshot.manufacturing_date.empty())
            cout << "\"" << json_escape(snapshot.manufacturing_date) << "\"";
        else cout << "null";
        cout << ",\"cable_length_raw\":";
        if (snapshot.has_optional("cable_length")) cout << snapshot.cable_length_raw;
        else cout << "null";
        cout << ",\"transfer_distance_m\":";
        if (snapshot.present && snapshot.has_optional("cable_length"))
            cout << snapshot.transfer_distance_m;
        else cout << "null";
        cout << ",\"cable_breakout_raw\":";
        if (snapshot.has_optional("cable_breakout")) cout << snapshot.cable_breakout_raw;
        else cout << "null";
        cout << ",\"smf_length_raw\":";
        if (snapshot.has_optional("smf_length")) cout << snapshot.smf_length_raw;
        else cout << "null";
        cout << ",\"attenuation_raw\":{\"5g\":";
        if (snapshot.has_optional("cable_attenuation_5g")) cout << snapshot.cable_attenuation_5g_raw; else cout << "null";
        cout << ",\"7g\":";
        if (snapshot.has_optional("cable_attenuation_7g")) cout << snapshot.cable_attenuation_7g_raw; else cout << "null";
        cout << ",\"12g\":";
        if (snapshot.has_optional("cable_attenuation_12g")) cout << snapshot.cable_attenuation_12g_raw; else cout << "null";
        cout << ",\"25g\":";
        if (snapshot.has_optional("cable_attenuation_25g")) cout << snapshot.cable_attenuation_25g_raw; else cout << "null";
        cout << ",\"53g\":";
        if (snapshot.has_optional("cable_attenuation_53g")) cout << snapshot.cable_attenuation_53g_raw; else cout << "null";
        cout << "},\"cable_rx_amp_raw\":";
        if (snapshot.has_optional("cable_rx_amp")) cout << snapshot.cable_rx_amp_raw; else cout << "null";
        cout << ",\"cable_rx_pre_emphasis_raw\":";
        if (snapshot.has_optional("cable_rx_emphasis")) cout << snapshot.cable_rx_pre_emphasis_raw; else cout << "null";
        cout << ",\"cable_rx_post_emphasis_raw\":";
        if (snapshot.has_optional("cable_rx_post_emphasis")) cout << snapshot.cable_rx_post_emphasis_raw; else cout << "null";
        cout << ",\"cable_tx_equalization_raw\":";
        if (snapshot.has_optional("cable_tx_equalization")) cout << snapshot.cable_tx_equalization_raw; else cout << "null";
        cout << ",\"wavelength_nm\":";
        if (snapshot.present && snapshot.wavelength_available)
            cout << snapshot.wavelength_nm;
        else
            cout << "null";
        cout << ",\"wavelength_tolerance_nm\":";
        if (snapshot.present && snapshot.wavelength_tolerance_available)
            cout << snapshot.wavelength_tolerance_nm;
        else cout << "null";
        cout << ",\"module_type\":" << snapshot.cable_type
             << ",\"module_type_name\":";
        if (!snapshot.cable_type_name.empty())
            cout << "\"" << json_escape(snapshot.cable_type_name) << "\"";
        else cout << "null";
        cout << ",\"cable_identifier\":" << snapshot.cable_identifier
             << ",\"cable_identifier_name\":";
        if (!snapshot.cable_identifier_name.empty())
            cout << "\"" << json_escape(snapshot.cable_identifier_name) << "\"";
        else cout << "null";
        cout << ",\"cable_technology\":" << snapshot.cable_technology
             << ",\"cable_technology_name\":";
        if (!snapshot.cable_technology_name.empty())
            cout << "\"" << json_escape(snapshot.cable_technology_name) << "\"";
        else cout << "null";
        cout << ",\"ethernet_compliance_code_raw\":";
        if (snapshot.has_optional("ethernet_compliance_code")) cout << snapshot.ethernet_compliance_code; else cout << "null";
        cout << ",\"ext_ethernet_compliance_code_raw\":";
        if (snapshot.has_optional("ext_ethernet_compliance_code")) cout << snapshot.ext_ethernet_compliance_code; else cout << "null";
        cout << ",\"active_set_host_compliance_code_raw\":";
        if (snapshot.has_optional("active_set_host_compliance_code")) cout << snapshot.active_set_host_compliance_code; else cout << "null";
        cout << ",\"active_set_media_compliance_code_raw\":";
        if (snapshot.has_optional("active_set_media_compliance_code")) cout << snapshot.active_set_media_compliance_code; else cout << "null";
        cout << ",\"ib_width_raw\":";
        if (snapshot.has_optional("ib_width")) cout << snapshot.ib_width_raw; else cout << "null";
        cout << ",\"nominal_bit_rate_100_raw\":";
        if (snapshot.has_optional("nbr100")) cout << snapshot.nominal_bit_rate_100_raw; else cout << "null";
        cout << ",\"nominal_bit_rate_250_raw\":";
        if (snapshot.has_optional("nbr250")) cout << snapshot.nominal_bit_rate_250_raw; else cout << "null";
        cout << ",\"fw_version_raw\":";
        if (snapshot.present && snapshot.fw_version_available)
            cout << snapshot.fw_version;
        else
            cout << "null";
        cout << ",\"fw_version\":";
        if (snapshot.present && snapshot.fw_version_available)
            cout << "\"" << snapshot.fw_version_string << "\"";
        else cout << "null";
        cout << "},\"status\":{\"presence\":\""
             << (snapshot.present ? "present" : "absent")
             << "\",\"module_state\":";
        if (snapshot.present && snapshot.module_state_available)
            cout << snapshot.module_state;
        else
            cout << "null";
        cout << ",\"module_state_name\":";
        if (snapshot.present && snapshot.module_state_available &&
            !snapshot.module_state_name.empty())
            cout << "\"" << json_escape(snapshot.module_state_name) << "\"";
        else cout << "null";
        cout << ",\"digital_diagnostic_monitoring\":";
        if (snapshot.has_optional("monitor_cap_mask"))
            cout << (snapshot.digital_diagnostic_monitoring ? "true" : "false");
        else cout << "null";
        cout << ",\"power_class_raw\":";
        if (snapshot.has_optional("cable_power_class")) cout << snapshot.cable_power_class; else cout << "null";
        cout << ",\"max_power_raw\":";
        if (snapshot.has_optional("max_power")) cout << snapshot.max_power_raw; else cout << "null";
        cout << ",\"max_power_w\":";
        if (snapshot.max_power_available) cout << snapshot.max_power_w;
        else cout << "null";
        cout << ",\"cdr_rx_cap_raw\":";
        if (snapshot.has_optional("rx_cdr_cap")) cout << snapshot.rx_cdr_cap; else cout << "null";
        cout << ",\"cdr_tx_cap_raw\":";
        if (snapshot.has_optional("tx_cdr_cap")) cout << snapshot.tx_cdr_cap; else cout << "null";
        cout << ",\"cdr_rx_state_mask\":";
        if (snapshot.has_optional("rx_cdr_state")) cout << snapshot.rx_cdr_state; else cout << "null";
        cout << ",\"cdr_tx_state_mask\":";
        if (snapshot.has_optional("tx_cdr_state")) cout << snapshot.tx_cdr_state; else cout << "null";
        cout << ",\"rx_power_type_raw\":";
        if (snapshot.has_optional("rx_power_type")) cout << snapshot.rx_power_type; else cout << "null";
        cout << ",\"rx_power_type\":";
        if (snapshot.has_optional("rx_power_type")) cout << "\"" << json_escape(snapshot.rx_power_type_name) << "\""; else cout << "null";
        cout << ",\"linear_direct_drive\":";
        if (snapshot.has_optional("did_cap")) cout << (snapshot.linear_direct_drive ? "true" : "false"); else cout << "null";
        cout << ",\"rx_output_valid_capable\":";
        if (snapshot.has_optional("rx_output_valid_cap"))
            cout << (snapshot.rx_output_valid_capable ? "true" : "false");
        else cout << "null";
        cout << ",\"error_code_raw\":";
        if (snapshot.has_optional("error_code")) cout << snapshot.error_code; else cout << "null";
        cout << ",\"error_code\":";
        if (snapshot.has_optional("error_code") && !snapshot.error_code_name.empty())
            cout << "\"" << json_escape(snapshot.error_code_name) << "\"";
        else cout << "null";
        cout << "},\"monitors\":{\"temperature_c\":";
        if (snapshot.present && snapshot.temperature_available)
            cout << snapshot.temperature_c;
        else
            cout << "null";
        cout << ",\"temperature_low_c\":";
        if (snapshot.present && snapshot.temperature_thresholds_available)
            cout << snapshot.temperature_low_c; else cout << "null";
        cout << ",\"temperature_high_c\":";
        if (snapshot.present && snapshot.temperature_thresholds_available)
            cout << snapshot.temperature_high_c; else cout << "null";
        cout << ",\"voltage_mv\":";
        if (snapshot.present && snapshot.voltage_available)
            cout << snapshot.voltage_mv;
        else
            cout << "null";
        cout << ",\"voltage_low_mv\":";
        if (snapshot.present && snapshot.voltage_thresholds_available)
            cout << snapshot.voltage_low_mv; else cout << "null";
        cout << ",\"voltage_high_mv\":";
        if (snapshot.present && snapshot.voltage_thresholds_available)
            cout << snapshot.voltage_high_mv; else cout << "null";
        cout << ",\"rx_power_low_uw\":";
        if (snapshot.present && snapshot.rx_power_thresholds_available)
            cout << snapshot.rx_power_low_uw; else cout << "null";
        cout << ",\"rx_power_high_uw\":";
        if (snapshot.present && snapshot.rx_power_thresholds_available)
            cout << snapshot.rx_power_high_uw; else cout << "null";
        cout << ",\"tx_power_low_uw\":";
        if (snapshot.present && snapshot.tx_power_thresholds_available)
            cout << snapshot.tx_power_low_uw; else cout << "null";
        cout << ",\"tx_power_high_uw\":";
        if (snapshot.present && snapshot.tx_power_thresholds_available)
            cout << snapshot.tx_power_high_uw; else cout << "null";
        cout << ",\"tx_bias_low_ua\":";
        if (snapshot.present && snapshot.tx_bias_thresholds_available)
            cout << snapshot.tx_bias_low_ua; else cout << "null";
        cout << ",\"tx_bias_high_ua\":";
        if (snapshot.present && snapshot.tx_bias_thresholds_available)
            cout << snapshot.tx_bias_high_ua; else cout << "null";
        cout << ",\"monitor_cap_mask\":";
        if (snapshot.has_optional("monitor_cap_mask")) cout << snapshot.monitor_cap_mask;
        else cout << "null";
        cout << "},\"media_lanes\":[";
        if (snapshot.present)
        {
            for (size_t lane_index = 0;
                 lane_index < snapshot.media_lanes.size(); ++lane_index)
            {
                print_media_lane_json(snapshot.media_lanes[lane_index]);
                if (lane_index + 1 != snapshot.media_lanes.size()) cout << ",";
            }
        }
        cout << "],\"updated_at\":";
        if (!module.updated_at.empty())
            cout << "\"" << module.updated_at << "\"";
        else
            cout << "null";
    }
    else
    {
        cout << ",\"actual_context\":null,\"present\":null,\"error\":\""
             << json_escape(module.error) << "\"";
    }
    cout << "}";
}

void print_channel_list(const vector<unsigned>& channels)
{
    cout << "[";
    for (size_t i = 0; i < channels.size(); ++i)
        cout << channels[i] << (i + 1 == channels.size() ? "" : ",");
    cout << "]";
}

void print_counter_available(const vector<AggregatedCounterSlot>& slots)
{
    cout << "[";
    for (size_t i = 0; i < slots.size(); ++i)
        cout << (slots[i].available ? "true" : "false")
             << (i + 1 == slots.size() ? "" : ",");
    cout << "]";
}

void print_counter_u64(const vector<AggregatedCounterSlot>& slots,
                       uint64_t PpcntValue::*member)
{
    cout << "[";
    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (slots[i].available) cout << slots[i].value.*member; else cout << "null";
        if (i + 1 != slots.size()) cout << ",";
    }
    cout << "]";
}

void print_counter_ber(const vector<AggregatedCounterSlot>& slots,
                       uint32_t PpcntValue::*coefficient,
                       uint32_t PpcntValue::*magnitude)
{
    cout << "[";
    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (slots[i].available)
            cout << slots[i].value.*coefficient << "e-" << slots[i].value.*magnitude;
        else
            cout << "null";
        if (i + 1 != slots.size()) cout << ",";
    }
    cout << "]";
}

void print_aggregated_ports_json(const vector<AggregatedPortResult>& ports)
{
    cout << "  \"aggregated_ports\": [\n";
    for (size_t port_index = 0; port_index < ports.size(); ++port_index)
    {
        const AggregatedPortResult& port = ports[port_index];
        cout << "    {\"label_port\":\"" << json_escape(port.label_port) << "\"";
        if (port.module_requested)
        {
            cout << ",\"module\":";
            if (port.module_result_available)
            {
                ModuleResult materialized = port.module_metadata;
                materialized.snapshot = port.module_snapshot;
                print_module_json(materialized);
            }
            else cout << "null";
        }
        cout << ",\"logical_ports\":[";
        for (size_t logical_index = 0;
             logical_index < port.logical_ports.size(); ++logical_index)
        {
            const AggregatedLogicalPort& logical = port.logical_ports[logical_index];
            cout << "{\"name\":\"" << json_escape(logical.name)
                 << "\",\"ipil\":" << logical.ipil
                 << ",\"logical_channels\":";
            print_channel_list(logical.channels);
            if (port.module_requested)
            {
                cout << ",\"module_lanes\":[";
                for (size_t lane_index = 0;
                     lane_index < logical.module_lanes.size(); ++lane_index)
                {
                    print_media_lane_json(logical.module_lanes[lane_index]);
                    if (lane_index + 1 != logical.module_lanes.size()) cout << ",";
                }
                cout << "]";
            }
            if (logical.counters_requested)
            {
                cout << ",\"counters\":{\"channels\":";
                print_channel_list(logical.channels);
                cout << ",\"available\":";
                print_counter_available(logical.counters);
                cout << ",\"time_since_last_clear_ms\":";
                print_counter_u64(logical.counters, &PpcntValue::time_since_last_clear_ms);
                cout << ",\"received_bits\":";
                print_counter_u64(logical.counters, &PpcntValue::received_bits);
                cout << ",\"symbol_errors\":";
                print_counter_u64(logical.counters, &PpcntValue::symbol_errors);
                cout << ",\"corrected_bits\":";
                print_counter_u64(logical.counters, &PpcntValue::corrected_bits);
                cout << ",\"raw_errors\":";
                print_counter_u64(logical.counters, &PpcntValue::raw_error);
                cout << ",\"effective_errors\":";
                print_counter_u64(logical.counters, &PpcntValue::effective_errors);
                cout << ",\"raw_ber\":";
                print_counter_ber(logical.counters, &PpcntValue::raw_ber_coefficient,
                                  &PpcntValue::raw_ber_magnitude);
                cout << ",\"raw_ber_lane0\":";
                print_counter_ber(logical.counters,
                                  &PpcntValue::raw_ber_lane0_coefficient,
                                  &PpcntValue::raw_ber_lane0_magnitude);
                cout << ",\"effective_ber\":";
                print_counter_ber(logical.counters,
                                  &PpcntValue::effective_ber_coefficient,
                                  &PpcntValue::effective_ber_magnitude);
                cout << ",\"symbol_ber\":";
                print_counter_ber(logical.counters, &PpcntValue::symbol_ber_coefficient,
                                  &PpcntValue::symbol_ber_magnitude);
                cout << "}";
            }
            if (logical.histogram_requested)
            {
                cout << ",\"histogram\":{\"channels\":";
                print_channel_list(logical.histogram.channels);
                cout << ",\"definition_consistent\":"
                     << (logical.histogram.definition_consistent ? "true" : "false")
                     << ",\"active_type\":";
                if (logical.histogram.active_type_available)
                    cout << logical.histogram.active_type;
                else
                    cout << "null";
                cout << ",\"bins\":[";
                for (size_t bin_index = 0;
                     bin_index < logical.histogram.bins.size(); ++bin_index)
                {
                    const AggregatedHistogramBin& bin =
                        logical.histogram.bins[bin_index];
                    cout << "{\"bin\":" << bin.index
                         << ",\"range\":{\"low\":" << bin.low
                         << ",\"high\":" << bin.high
                         << "},\"occurrences\":[";
                    for (size_t channel_index = 0;
                         channel_index < bin.occurrences.size(); ++channel_index)
                    {
                        if (bin.occurrence_available[channel_index])
                            cout << bin.occurrences[channel_index];
                        else
                            cout << "null";
                        if (channel_index + 1 != bin.occurrences.size()) cout << ",";
                    }
                    cout << "]}" << (bin_index + 1 == logical.histogram.bins.size() ? "" : ",");
                }
                cout << "]}";
            }
            cout << "}" << (logical_index + 1 == port.logical_ports.size() ? "" : ",");
        }
        cout << "]}" << (port_index + 1 == ports.size() ? "\n" : ",\n");
    }
    cout << "  ]";
}

void print_scan_json(const ScanResult& result)
{
    cout << fixed << setprecision(3)
         << "{\n  \"requested\": " << result.objects.size()
         << ",\n  \"success\": " << result.success_count
         << ",\n  \"failed\": " << result.failed_count
         << ",\n  \"module_requested\": " << result.modules.size()
         << ",\n  \"module_success\": " << result.module_success_count
         << ",\n  \"module_absent\": " << result.module_absent_count
         << ",\n  \"module_failed\": " << result.module_failed_count
         << ",\n  \"cold_start_ms\": " << result.cold_start_ms
         << ",\n  \"channel_scan_ms\": " << result.channel_scan_ms
         << ",\n  \"module_scan_ms\": " << result.module_scan_ms
         << ",\n  \"scan_ms\": " << result.scan_ms
         << ",\n  \"total_ms\": " << result.total_ms
         << ",\n  \"objects\": [\n";
    for (size_t i = 0; i < result.objects.size(); ++i)
    {
        const ObjectResult& object = result.objects[i];
        cout << "    {\"port\":\"" << json_escape(object.topology.label_port)
             << "\",\"interface\":\"sw" << json_escape(object.topology.label_port)
             << "p" << object.topology.ipil
             << "\",\"channel\":" << object.topology.channel
             << ",\"ipil\":" << object.topology.ipil
             << ",\"mapping_channel\":" << object.topology.mapping_channel
             << ",\"split\":" << object.topology.split
             << ",\"pciconf\":\"" << object.topology.pciconf
             << "\",\"bdf\":\"" << object.topology.bdf
             << "\",\"local_port\":" << object.topology.local_port
             << ",\"module\":\"" << json_escape(object.topology.module)
             << "\",\"sub_module\":\"" << json_escape(object.topology.sub_module)
             << "\",\"success\":" << (object.success ? "true" : "false");
        if (object.counters_success)
        {
            cout << ",\"ppcnt\":{\"time_since_last_clear_ms\":"
                     << object.ppcnt.time_since_last_clear_ms
                     << ",\"received_bits\":" << object.ppcnt.received_bits
                     << ",\"symbol_errors\":" << object.ppcnt.symbol_errors
                     << ",\"corrected_bits\":" << object.ppcnt.corrected_bits
                     << ",\"raw_error\":" << object.ppcnt.raw_error
                     << ",\"effective_errors\":" << object.ppcnt.effective_errors
                     << ",\"raw_ber\":" << object.ppcnt.raw_ber_coefficient
                     << "e-" << object.ppcnt.raw_ber_magnitude
                     << ",\"raw_ber_floor\":"
                     << ((object.ppcnt.raw_ber_coefficient == 15 &&
                          object.ppcnt.raw_ber_magnitude == 255) ? "true" : "false")
                     << ",\"raw_ber_coefficient\":" << object.ppcnt.raw_ber_coefficient
                     << ",\"raw_ber_magnitude\":" << object.ppcnt.raw_ber_magnitude
                     << ",\"raw_ber_lane0\":" << object.ppcnt.raw_ber_lane0_coefficient
                     << "e-" << object.ppcnt.raw_ber_lane0_magnitude
                     << ",\"raw_ber_lane0_coefficient\":"
                     << object.ppcnt.raw_ber_lane0_coefficient
                     << ",\"raw_ber_lane0_magnitude\":"
                     << object.ppcnt.raw_ber_lane0_magnitude
                     << ",\"effective_ber\":" << object.ppcnt.effective_ber_coefficient
                     << "e-" << object.ppcnt.effective_ber_magnitude
                     << ",\"effective_ber_coefficient\":"
                     << object.ppcnt.effective_ber_coefficient
                     << ",\"effective_ber_magnitude\":"
                     << object.ppcnt.effective_ber_magnitude
                     << ",\"symbol_ber\":" << object.ppcnt.symbol_ber_coefficient
                     << "e-" << object.ppcnt.symbol_ber_magnitude
                     << ",\"symbol_ber_coefficient\":"
                     << object.ppcnt.symbol_ber_coefficient
                     << ",\"symbol_ber_magnitude\":"
                 << object.ppcnt.symbol_ber_magnitude << "}";
        }
        if (object.histogram_success)
        {
            cout << ",\"histogram\":{\"active_type\":"
                     << object.histogram.active_type << ",\"bins\":[";
                for (size_t bin_index = 0;
                     bin_index < object.histogram.bins.size(); ++bin_index)
                {
                    const HistogramBin& bin = object.histogram.bins[bin_index];
                    cout << "{\"index\":" << bin.index
                         << ",\"low\":" << bin.low
                         << ",\"high\":" << bin.high
                         << ",\"count\":" << bin.count << "}"
                         << (bin_index + 1 == object.histogram.bins.size() ? "" : ",");
                }
            cout << "]}";
        }
        if (!object.success)
            cout << ",\"error\":\"" << json_escape(object.error) << "\"";
        cout << "}" << (i + 1 == result.objects.size() ? "\n" : ",\n");
    }
    cout << "  ],\n  \"modules\": [\n";
    for (size_t i = 0; i < result.modules.size(); ++i)
    {
        cout << "    ";
        print_module_json(result.modules[i]);
        cout << (i + 1 == result.modules.size() ? "\n" : ",\n");
    }
    cout << "  ],\n";
    print_aggregated_ports_json(result.aggregated_ports);
    cout << "\n}\n";
}

struct Options
{
    string mapping;
    string adb;
    string ports = "all";
    string channels = "all";
    bool counters = true;
    bool histogram = false;
    bool modules = false;
    bool read = false;
};

void print_usage(const char* program)
{
    cerr << "Temporary/test CLI (not the final product interface):\n"
         << "  " << program << " --self-test-aggregation\n"
         << "  " << program << " --mapping FILE [--ports SPEC] [--channels SPEC]\n"
         << "  " << program << " --mapping FILE --adb FILE --read [--ports SPEC] [--channels SPEC]"
         << " [--capabilities counters|histogram|module|comma-separated combination]\n\n"
         << "Temporary physical label_port examples: all | 1 | 1,5 | 1-16\n"
         << "Channel examples: all | 1 | 1,2,5 | CH1,CH2\n"
         << "Without --read, only mapping and selection are validated; no device is opened.\n";
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const string argument = argv[i];
        if (argument == "--read")
            options.read = true;
        else if (argument == "--mapping" || argument == "--adb" ||
                 argument == "--ports" || argument == "--channels" ||
                 argument == "--capabilities")
        {
            if (++i >= argc)
                throw invalid_argument("missing value for " + argument);
            const string value = argv[i];
            if (argument == "--mapping") options.mapping = value;
            else if (argument == "--adb") options.adb = value;
            else if (argument == "--ports") options.ports = value;
            else if (argument == "--channels") options.channels = value;
            else
            {
                options.counters = false;
                options.histogram = false;
                options.modules = false;
                for (const string& capability : split(value, ','))
                {
                    if (capability == "counters") options.counters = true;
                    else if (capability == "histogram") options.histogram = true;
                    else if (capability == "module") options.modules = true;
                    else throw invalid_argument("unknown capability: " + capability);
                }
                if (!options.counters && !options.histogram && !options.modules)
                    throw invalid_argument("--capabilities must not be empty");
            }
        }
        else
            throw invalid_argument("unknown argument: " + argument);
    }
    if (options.mapping.empty())
        throw invalid_argument("--mapping is required");
    if (options.read && options.adb.empty())
        throw invalid_argument("--adb is required with --read");
    return options;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 1)
    {
        print_usage(argv[0]);
        return 1;
    }

    try
    {
        if (argc == 2 && string(argv[1]) == "--self-test-aggregation")
        {
            run_aggregation_self_test();
            return 0;
        }
        const Options options = parse_options(argc, argv);
        const Mapping mapping(options.mapping);
        const set<string> ports = parse_ports(options.ports);
        const set<unsigned> channels = parse_channels(options.channels);
        const vector<MappingEntry> selected = mapping.select(ports, channels);
        const vector<ModulePlan> module_plans = options.modules ?
            build_module_plans(mapping.entries(), selected) : vector<ModulePlan>();

        if (!options.read)
        {
            map<string, size_t> buckets;
            for (const MappingEntry& entry : selected)
                ++buckets[entry.pciconf];
            cout << "mapping_objects=" << mapping.entries().size() << "\n"
                 << "selected_objects=" << selected.size() << "\n";
            for (const auto& bucket : buckets)
                cout << bucket.first << "_objects=" << bucket.second << "\n";
            cout << "selected_modules=" << module_plans.size() << "\n";
            for (const ModulePlan& plan : module_plans)
            {
                const MappingEntry& canonical = plan.contexts.front();
                cout << "module_plan=" << plan.label_port
                     << ",contexts=" << plan.contexts.size()
                     << ",canonical_channel=" << canonical.channel
                     << ",canonical_pciconf=" << canonical.pciconf
                     << ",canonical_local_port=" << canonical.local_port << "\n";
            }
            cout << "device_access=disabled\n";
            return 0;
        }

        const Clock::time_point total_start = Clock::now();
        Reader reader(options.adb, selected, module_plans,
                      options.counters, options.histogram, options.modules);
        const Clock::time_point scan_ready = Clock::now();
        const double cold_start_ms =
            chrono::duration<double, milli>(scan_ready - total_start).count();
        const ScanResult result = reader.read_once(cold_start_ms);
        print_scan_json(result);
        return result.failed_count == 0 && result.module_failed_count == 0 ? 0 : 2;
    }
    catch (const exception& error)
    {
        cerr << "q3400_reader: " << error.what() << "\n";
        return 1;
    }
}
