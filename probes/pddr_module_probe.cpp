/*
 * One-shot, GET-only PDDR module-info probe for Q3400 experiments.
 *
 * This is intentionally separate from q3400_reader.  It issues exactly one
 * PDDR page 3 GET, validates the response header, and prints the firmware-
 * decoded module/DDM fields needed to compare Q3400 access contexts.
 */

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
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

const unsigned MAX_LOCAL_PORT = 254;
using ProbeAdbInstance = _AdbInstance_impl<true, uint32_t>;
using FieldMap = map<string, ProbeAdbInstance*>;

bool valid_bdf(const string& device)
{
    static const regex pattern(
        "^([[:xdigit:]]{4}:)?([[:xdigit:]]{2}):([[:xdigit:]]{2})\\.([0-7])$");
    smatch match;
    if (!regex_match(device, match, pattern))
        return false;
    return strtoul(match[3].str().c_str(), nullptr, 16) <= 0x1f;
}

unsigned parse_unsigned(const char* text, const char* name,
                        unsigned minimum, unsigned maximum)
{
    if (!text || !*text || *text == '-' || *text == '+')
        throw invalid_argument(string(name) + " must be an unsigned decimal integer");
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        value < minimum || value > maximum)
    {
        ostringstream error;
        error << name << " must be in range " << minimum << ".." << maximum;
        throw invalid_argument(error.str());
    }
    return static_cast<unsigned>(value);
}

void require_adb(const string& path)
{
    struct stat status;
    if (path.empty() || stat(path.c_str(), &status) != 0)
        throw invalid_argument("ADB file does not exist: " + path);
    if (!S_ISREG(status.st_mode) || status.st_size <= 0)
        throw invalid_argument("ADB must be a non-empty regular file: " + path);
    if (access(path.c_str(), R_OK) != 0)
        throw invalid_argument("ADB is not readable: " + path);
}

bool capture_field(const string& path, uint64_t, uint64_t,
                   ProbeAdbInstance* instance, void* opaque)
{
    if (!instance || !instance->fieldDesc)
        return false;
    FieldMap& fields = *static_cast<FieldMap*>(opaque);
    const string name = instance->fieldDesc->name;
    fields.insert(make_pair(name, instance));
    fields.insert(make_pair(path, instance));
    return false;
}

ProbeAdbInstance* field(const FieldMap& fields, const string& name)
{
    FieldMap::const_iterator exact = fields.find(name);
    if (exact != fields.end())
        return exact->second;
    const string suffix = "." + name;
    for (FieldMap::const_iterator it = fields.begin(); it != fields.end(); ++it)
    {
        if (it->first.size() >= suffix.size() &&
            it->first.compare(it->first.size() - suffix.size(), suffix.size(), suffix) == 0)
            return it->second;
    }
    throw runtime_error("PDDR page 3 layout is missing field: " + name);
}

uint64_t pop(const FieldMap& fields, const string& name,
             const vector<u_int32_t>& buffer)
{
    return field(fields, name)->popBuf(reinterpret_cast<uint8_t*>(
        const_cast<u_int32_t*>(&buffer[0])));
}

string json_escape(const string& value)
{
    ostringstream out;
    for (unsigned char c : value)
    {
        switch (c)
        {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20)
                out << "\\u" << hex << setw(4) << setfill('0')
                    << static_cast<unsigned>(c) << dec << setfill(' ');
            else
                out << static_cast<char>(c);
        }
    }
    return out.str();
}

uint64_t fnv1a(const vector<uint64_t>& values)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint64_t value : values)
    {
        for (unsigned i = 0; i < 8; ++i)
        {
            hash ^= static_cast<uint8_t>(value >> (i * 8));
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

string hex64(uint64_t value)
{
    ostringstream out;
    out << hex << setw(16) << setfill('0') << value;
    return out.str();
}

double elapsed_ms(const Clock::time_point& start, const Clock::time_point& end)
{
    return chrono::duration<double, milli>(end - start).count();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4 && argc != 5)
    {
        cerr << "Usage:\n"
             << "  " << argv[0] << " <PCI_BDF> <ADB_file> <local_port>\n"
             << "  " << argv[0] << " <PCI_BDF> <ADB_file> pddr9 <local_port>\n"
             << "  " << argv[0] << " <PCI_BDF> <ADB_file> pmaos <module>\n"
             << "Every mode issues exactly one GET; no SET path exists.\n";
        return 1;
    }

    const string device = argv[1];
    const string adb_file = argv[2];
    const string mode = argc == 4 ? "pddr3" : argv[3];
    unsigned index = 0;
    try
    {
        if (!valid_bdf(device))
            throw invalid_argument("device must be a PCI BDF such as 05:00.0");
        require_adb(adb_file);
        if (mode == "pddr3")
            index = parse_unsigned(argv[3], "local_port", 0, MAX_LOCAL_PORT);
        else if (mode == "pddr9")
            index = parse_unsigned(argv[4], "local_port", 0, MAX_LOCAL_PORT);
        else if (mode == "pmaos")
            index = parse_unsigned(argv[4], "module", 0, 255);
        else
            throw invalid_argument("mode must be pddr9 or pmaos");
    }
    catch (const exception& error)
    {
        cerr << "Invalid arguments: " << error.what() << "\n";
        return 1;
    }

    mfile* mf = nullptr;
    try
    {
        const Clock::time_point total_start = Clock::now();
        mf = mopen(device.c_str());
        if (!mf)
            throw runtime_error("mopen failed for " + device);

        MlxRegLib reg(mf, adb_file, true);
        if (mode != "pddr3")
        {
            const string register_name = mode == "pddr9" ? "PDDR" : "PMAOS";
            ProbeAdbInstance* status_node = reg.findAdbNode(register_name);
            if (!status_node)
                throw runtime_error(register_name + " not found in ADB");
            const uint32_t status_bytes =
                static_cast<uint32_t>(status_node->get_size() / 8);
            if (status_bytes == 0 || status_bytes % sizeof(u_int32_t) != 0)
                throw runtime_error("invalid " + register_name + " register size");

            const string status_indexes = mode == "pddr9"
                ? "local_port=" + to_string(index) +
                  ",lp_msb=0,pnat=0,plane_ind=0,port_type=0,module_ind_type=0,page_select=9"
                : "module=" + to_string(index) + ",slot_index=0";
            const string status_ops = mode == "pddr9" ? "module_info_ext=1" : "";
            RegAccessParser status_parser("", status_indexes, status_ops,
                                           &reg.getAdb(), status_node,
                                           status_bytes, false, false);
            const vector<u_int32_t> status_request = status_parser.genBuff();
            FieldMap request_status_fields;
            reg.getAdb().traverse_layout(
                status_node, "", 0,
                reinterpret_cast<const uint8_t*>(&status_request[0]), status_bytes,
                capture_field, &request_status_fields, true, false, true);
            if (mode == "pddr9")
            {
                if (pop(request_status_fields, "local_port", status_request) != index ||
                    pop(request_status_fields, "page_select", status_request) != 9 ||
                    pop(request_status_fields, "pnat", status_request) != 0)
                    throw runtime_error("malformed PDDR page 9 GET template");
            }
            else if (pop(request_status_fields, "module", status_request) != index ||
                     pop(request_status_fields, "slot_index", status_request) != 0)
                throw runtime_error("malformed PMAOS GET template");

            vector<u_int32_t> status_response = status_request;
            const Clock::time_point status_get_start = Clock::now();
            reg.sendRegister(register_name, MACCESS_REG_METHOD_GET, status_response);
            const Clock::time_point status_get_end = Clock::now();
            FieldMap status_fields;
            reg.getAdb().traverse_layout(
                status_node, "", 0,
                reinterpret_cast<const uint8_t*>(&status_response[0]), status_bytes,
                capture_field, &status_fields, true, false, true);

            cout << fixed << setprecision(3)
                 << "{\n  \"register\": \"" << register_name << "\",\n"
                 << "  \"method\": \"GET\",\n"
                 << "  \"response_bytes\": " << status_bytes << ",\n"
                 << "  \"get_ms\": " << elapsed_ms(status_get_start, status_get_end) << ",\n";
            if (mode == "pddr9")
            {
                if (pop(status_fields, "local_port", status_response) != index ||
                    pop(status_fields, "page_select", status_response) != 9)
                    throw runtime_error("PDDR page 9 response header mismatch");
                cout << "  \"local_port\": " << index << ",\n"
                     << "  \"mod_fw_fault\": " << pop(status_fields, "mod_fw_fault", status_response) << ",\n"
                     << "  \"dp_fw_fault\": " << pop(status_fields, "dp_fw_fault", status_response) << ",\n"
                     << "  \"temp_flags\": " << pop(status_fields, "temp_flags", status_response) << ",\n"
                     << "  \"vcc_flags\": " << pop(status_fields, "vcc_flags", status_response) << ",\n"
                     << "  \"tx_fault_mask\": " << pop(status_fields, "tx_fault", status_response) << ",\n"
                     << "  \"tx_los_mask\": " << pop(status_fields, "tx_los", status_response) << ",\n"
                     << "  \"rx_los_mask\": " << pop(status_fields, "rx_los", status_response) << ",\n"
                     << "  \"tx_cdr_lol_mask\": " << pop(status_fields, "tx_cdr_lol", status_response) << ",\n"
                     << "  \"rx_cdr_lol_mask\": " << pop(status_fields, "rx_cdr_lol", status_response) << "\n";
            }
            else
            {
                if (pop(status_fields, "module", status_response) != index)
                    throw runtime_error("PMAOS response module mismatch");
                cout << "  \"module\": " << index << ",\n"
                     << "  \"oper_status\": " << pop(status_fields, "oper_status", status_response) << ",\n"
                     << "  \"error_type\": " << pop(status_fields, "error_type", status_response) << ",\n"
                     << "  \"secondary\": " << pop(status_fields, "secondary", status_response) << "\n";
            }
            cout << "}\n";
            mclose(mf);
            return 0;
        }

        const unsigned local_port = index;
        q3400::PddrModuleSnapshotReader module_reader(reg);
        const vector<u_int32_t> request = module_reader.build_request(local_port);
        const Clock::time_point get_start = Clock::now();
        const q3400::ModuleSnapshotData snapshot =
            module_reader.read(local_port, request);
        const Clock::time_point get_end = Clock::now();

        vector<uint64_t> scalar_values = {
            snapshot.cable_identifier, snapshot.cable_type, snapshot.cable_technology,
            snapshot.module_state, snapshot.fw_version, snapshot.memory_map_revision,
            snapshot.memory_map_compliance, snapshot.wavelength_nm,
            snapshot.monitor_cap_mask
        };
        for (char c : snapshot.vendor + "\0" + snapshot.part_number + "\0" +
                      snapshot.serial_number + "\0" + snapshot.revision)
            scalar_values.push_back(static_cast<unsigned char>(c));

        vector<uint64_t> lane_values;
        unsigned valid_lane_count = 0;
        for (const q3400::ModuleMediaLane& lane : snapshot.media_lanes)
        {
            if (lane.datapath_state_available)
                ++valid_lane_count;
            lane_values.push_back(lane.rx_power_uw);
            lane_values.push_back(lane.tx_power_uw);
            lane_values.push_back(lane.tx_bias_raw);
            lane_values.push_back(lane.datapath_state);
            lane_values.push_back(lane.rx_output_valid);
        }
        const Clock::time_point total_end = Clock::now();

        cout << fixed << setprecision(3);
        cout << "{\n"
             << "  \"register\": \"PDDR\",\n"
             << "  \"method\": \"GET\",\n"
             << "  \"page\": 3,\n"
             << "  \"device\": \"" << json_escape(device) << "\",\n"
             << "  \"local_port\": " << local_port << ",\n"
             << "  \"response_bytes\": " << module_reader.response_bytes() << ",\n"
             << "  \"get_ms\": " << elapsed_ms(get_start, get_end) << ",\n"
             << "  \"total_ms\": " << elapsed_ms(total_start, total_end) << ",\n"
             << "  \"scalar_hash\": \"" << hex64(fnv1a(scalar_values)) << "\",\n"
             << "  \"lane_ddm_hash\": \"" << hex64(fnv1a(lane_values)) << "\",\n"
             << "  \"valid_lane_count_from_dp_state\": " << valid_lane_count << ",\n"
             << "  \"module\": {\n"
             << "    \"present\": " << (snapshot.present ? "true" : "false") << ",\n"
             << "    \"vendor\": \"" << json_escape(snapshot.vendor) << "\",\n"
             << "    \"part_number\": \"" << json_escape(snapshot.part_number) << "\",\n"
             << "    \"serial_number\": \"" << json_escape(snapshot.serial_number) << "\",\n"
             << "    \"revision\": \"" << json_escape(snapshot.revision) << "\",\n"
             << "    \"cable_identifier\": " << snapshot.cable_identifier << ",\n"
             << "    \"cable_type\": " << snapshot.cable_type << ",\n"
             << "    \"cable_technology\": " << snapshot.cable_technology << ",\n"
             << "    \"module_state\": " << snapshot.module_state << ",\n"
             << "    \"fw_version_raw\": " << snapshot.fw_version << ",\n"
             << "    \"memory_map_revision\": " << snapshot.memory_map_revision << ",\n"
             << "    \"memory_map_compliance_raw\": " << snapshot.memory_map_compliance << ",\n"
             << "    \"wavelength_nm\": " << snapshot.wavelength_nm << ",\n"
             << "    \"temperature_c\": " << snapshot.temperature_c << ",\n"
             << "    \"voltage_mv\": " << snapshot.voltage_mv << ",\n"
             << "    \"monitor_cap_mask\": " << snapshot.monitor_cap_mask << "\n"
             << "  },\n"
             << "  \"lanes\": [\n";
        for (size_t i = 0; i < snapshot.media_lanes.size(); ++i)
        {
            const q3400::ModuleMediaLane& lane = snapshot.media_lanes[i];
            cout << "    {\"lane\": " << lane.lane
                 << ", \"dp_state\": " << lane.datapath_state
                 << ", \"rx_power_raw_uw\": " << lane.rx_power_uw
                 << ", \"tx_power_raw_uw\": " << lane.tx_power_uw
                 << ", \"tx_bias_raw\": " << lane.tx_bias_raw << "}"
                 << (i + 1 == snapshot.media_lanes.size() ? "\n" : ",\n");
        }
        cout << "  ]\n}\n";

        mclose(mf);
        return 0;
    }
    catch (const exception& error)
    {
        if (mf)
            mclose(mf);
        cerr << "PDDR probe failed: " << error.what() << "\n";
        return 2;
    }
}
