/*
 * One-shot, GET-only FEC histogram probe for Q3400 validation.
 *
 * Exactly two Access Register transactions are issued: one PPHCR GET for the
 * active bin layout and one PPCNT group 0x23 GET for counters.  There is no
 * SET or clear mode in this program; PPCNT clr is explicitly fixed at zero.
 */

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
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

using namespace std;
using namespace mlxreg;
using Clock = chrono::steady_clock;

namespace
{

const unsigned MAX_LOCAL_PORT = 254;
const uint32_t HISTOGRAM_GROUP = 0x23;
const unsigned MAX_PPHCR_BINS = 19;

using ProbeAdbInstance = _AdbInstance_impl<true, uint32_t>;
using FieldMap = map<string, ProbeAdbInstance*>;

bool valid_bdf(const string& device)
{
    static const regex pattern(
        "^([[:xdigit:]]{4}:)?([[:xdigit:]]{2}):([[:xdigit:]]{2})\\.([0-7])$");
    smatch match;
    return regex_match(device, match, pattern) &&
           strtoul(match[3].str().c_str(), nullptr, 16) <= 0x1f;
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
    fields.insert(make_pair(instance->fieldDesc->name, instance));
    fields.insert(make_pair(path, instance));
    return false;
}

ProbeAdbInstance* find_field(const FieldMap& fields,
                             const vector<string>& candidates,
                             const string& description)
{
    for (const string& candidate : candidates)
    {
        FieldMap::const_iterator exact = fields.find(candidate);
        if (exact != fields.end())
            return exact->second;
        const string suffix = "." + candidate;
        for (FieldMap::const_iterator it = fields.begin(); it != fields.end(); ++it)
        {
            if (it->first.size() >= suffix.size() &&
                it->first.compare(it->first.size() - suffix.size(), suffix.size(), suffix) == 0)
                return it->second;
        }
    }
    throw runtime_error("layout is missing field: " + description);
}

uint64_t pop(const FieldMap& fields, const vector<string>& names,
             const string& description, const vector<u_int32_t>& buffer)
{
    return find_field(fields, names, description)->popBuf(reinterpret_cast<uint8_t*>(
        const_cast<u_int32_t*>(&buffer[0])));
}

uint64_t pop(const FieldMap& fields, const string& name,
             const vector<u_int32_t>& buffer)
{
    return pop(fields, vector<string>(1, name), name, buffer);
}

void capture_layout(MlxRegLib& reg, ProbeAdbInstance* node,
                    const vector<u_int32_t>& buffer, FieldMap& fields)
{
    reg.getAdb().traverse_layout(
        node, "", 0, reinterpret_cast<const uint8_t*>(&buffer[0]),
        static_cast<uint32_t>(buffer.size() * sizeof(u_int32_t)),
        capture_field, &fields, true, false, true);
}

uint64_t histogram_word(const FieldMap& fields, unsigned bin, bool high,
                        const vector<u_int32_t>& buffer)
{
    const string index = to_string(bin);
    const string half = high ? "hi" : "lo";
    return pop(fields,
               {"hist[" + index + "]." + half,
                "hist[" + index + "]_" + half,
                "hist_" + index + "." + half},
               "hist[" + index + "]." + half, buffer);
}

uint64_t bin_limit(const FieldMap& fields, unsigned bin, bool high,
                   const vector<u_int32_t>& buffer)
{
    const string index = to_string(bin);
    const string name = high ? "high_val" : "low_val";
    return pop(fields,
               {"bin_range[" + index + "]." + name,
                name + "_" + index,
                "bin_range_" + index + "." + name},
               "bin_range[" + index + "]." + name, buffer);
}

double elapsed_ms(const Clock::time_point& start, const Clock::time_point& end)
{
    return chrono::duration<double, milli>(end - start).count();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        cerr << "Usage: " << argv[0] << " <PCI_BDF> <ADB_file> <local_port>\n"
             << "Issues exactly PPHCR GET + PPCNT group 0x23 GET; no SET/clear path exists.\n";
        return 1;
    }

    mfile* mf = nullptr;
    try
    {
        const string device = argv[1];
        const string adb_file = argv[2];
        if (!valid_bdf(device))
            throw invalid_argument("device must be a PCI BDF such as 05:00.0");
        require_adb(adb_file);
        const unsigned local_port =
            parse_unsigned(argv[3], "local_port", 0, MAX_LOCAL_PORT);

        mf = mopen(device.c_str());
        if (!mf)
            throw runtime_error("mopen failed for " + device);
        MlxRegLib reg(mf, adb_file, true);

        ProbeAdbInstance* pphcr_node = reg.findAdbNode("PPHCR");
        ProbeAdbInstance* ppcnt_node = reg.findAdbNode("PPCNT");
        if (!pphcr_node || !ppcnt_node)
            throw runtime_error("PPHCR or PPCNT not found in ADB");

        const uint32_t pphcr_bytes = static_cast<uint32_t>(pphcr_node->get_size() / 8);
        const uint32_t ppcnt_bytes = static_cast<uint32_t>(ppcnt_node->get_size() / 8);
        if (pphcr_bytes != 92 || ppcnt_bytes != 256 ||
            pphcr_bytes % 4 != 0 || ppcnt_bytes % 4 != 0)
            throw runtime_error("unexpected PPHCR/PPCNT response size");

        const string pphcr_indexes =
            "local_port=" + to_string(local_port) +
            ",lp_msb=0,pnat=0,plane_ind=0,port_type=0,hist_type=0";
        RegAccessParser pphcr_parser("", pphcr_indexes, "", &reg.getAdb(),
                                     pphcr_node, pphcr_bytes, false, false);
        vector<u_int32_t> pphcr = pphcr_parser.genBuff();
        FieldMap pphcr_fields;
        capture_layout(reg, pphcr_node, pphcr, pphcr_fields);
        if (pop(pphcr_fields, "local_port", pphcr) != local_port ||
            pop(pphcr_fields, "hist_type", pphcr) != 0)
            throw runtime_error("unsafe or invalid PPHCR request template");

        const string ppcnt_indexes =
            "grp=0x23,local_port=" + to_string(local_port) +
            ",lp_msb=0,pnat=0,plane_ind=0,port_type=0,lp_gl=1";
        RegAccessParser ppcnt_parser("", ppcnt_indexes, "clr=0", &reg.getAdb(),
                                     ppcnt_node, ppcnt_bytes, false, false);
        vector<u_int32_t> ppcnt = ppcnt_parser.genBuff();
        FieldMap ppcnt_fields;
        capture_layout(reg, ppcnt_node, ppcnt, ppcnt_fields);
        if (pop(ppcnt_fields, "grp", ppcnt) != HISTOGRAM_GROUP ||
            pop(ppcnt_fields, "local_port", ppcnt) != local_port ||
            pop(ppcnt_fields, "clr", ppcnt) != 0)
            throw runtime_error("unsafe or invalid PPCNT request template");

        const Clock::time_point pphcr_start = Clock::now();
        reg.sendRegister("PPHCR", MACCESS_REG_METHOD_GET, pphcr);
        const Clock::time_point pphcr_end = Clock::now();

        if (pphcr.size() * sizeof(u_int32_t) != pphcr_bytes ||
            pop(pphcr_fields, "local_port", pphcr) != local_port ||
            pop(pphcr_fields, "hist_type", pphcr) != 0)
            throw runtime_error("PPHCR response validation failed");
        const unsigned active_hist_type =
            static_cast<unsigned>(pop(pphcr_fields, "active_hist_type", pphcr));
        const unsigned num_bins =
            static_cast<unsigned>(pop(pphcr_fields, "num_of_bins", pphcr));
        if (active_hist_type == 0)
            throw runtime_error("no histogram is active for this link/FEC");
        if (num_bins == 0 || num_bins > MAX_PPHCR_BINS)
            throw runtime_error("PPHCR returned invalid num_of_bins=" + to_string(num_bins));

        const Clock::time_point ppcnt_start = Clock::now();
        reg.sendRegister("PPCNT", MACCESS_REG_METHOD_GET, ppcnt);
        const Clock::time_point ppcnt_end = Clock::now();

        if (ppcnt.size() * sizeof(u_int32_t) != ppcnt_bytes ||
            pop(ppcnt_fields, "grp", ppcnt) != HISTOGRAM_GROUP ||
            pop(ppcnt_fields, "local_port", ppcnt) != local_port ||
            pop(ppcnt_fields, "clr", ppcnt) != 0)
            throw runtime_error("PPCNT histogram response validation failed");

        cout << fixed << setprecision(3)
             << "{\n  \"method\": \"GET_ONLY\","
             << "\n  \"device\": \"" << device << "\","
             << "\n  \"local_port\": " << local_port << ","
             << "\n  \"pphcr_response_bytes\": " << pphcr_bytes << ","
             << "\n  \"ppcnt_response_bytes\": " << ppcnt_bytes << ","
             << "\n  \"pphcr_get_ms\": " << elapsed_ms(pphcr_start, pphcr_end) << ","
             << "\n  \"ppcnt_get_ms\": " << elapsed_ms(ppcnt_start, ppcnt_end) << ","
             << "\n  \"active_hist_type\": " << active_hist_type << ","
             << "\n  \"num_bins\": " << num_bins << ","
             << "\n  \"bins\": [\n";
        for (unsigned bin = 0; bin < num_bins; ++bin)
        {
            const uint64_t count =
                (histogram_word(ppcnt_fields, bin, true, ppcnt) << 32) |
                histogram_word(ppcnt_fields, bin, false, ppcnt);
            cout << "    {\"index\":" << bin
                 << ",\"low\":" << bin_limit(pphcr_fields, bin, false, pphcr)
                 << ",\"high\":" << bin_limit(pphcr_fields, bin, true, pphcr)
                 << ",\"count\":" << count << "}"
                 << (bin + 1 == num_bins ? "\n" : ",\n");
        }
        cout << "  ]\n}\n";

        mclose(mf);
        return 0;
    }
    catch (const exception& error)
    {
        if (mf)
            mclose(mf);
        cerr << "fec_histogram_probe: " << error.what() << "\n";
        return 1;
    }
}
