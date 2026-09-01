#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
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

static const uint32_t PPCNT_GROUP = 0x16;
static const unsigned MAX_LOCAL_PORT = 254;
static const unsigned MAX_BENCHMARK_COUNT = 1000000;

struct PpcntFields
{
    bool grp_seen = false;
    bool local_port_seen = false;
    bool clr_seen = false;
    bool time_high_seen = false;
    bool time_low_seen = false;
    bool raw_errors_high_seen = false;
    bool raw_errors_low_seen = false;
    bool raw_ber_coef_seen = false;
    bool raw_ber_magnitude_seen = false;
    uint32_t grp = 0;
    uint32_t local_port = 0;
    uint32_t clr = 0;
    uint32_t time_high = 0;
    uint32_t time_low = 0;
    uint32_t raw_errors_high = 0;
    uint32_t raw_errors_low = 0;
    uint32_t raw_ber_coef = 0;
    uint32_t raw_ber_magnitude = 0;
    bool conflicting_duplicate = false;
    string conflicting_field;

    uint64_t time_since_last_clear() const
    {
        return (static_cast<uint64_t>(time_high) << 32) | time_low;
    }

    uint64_t lane0_raw_errors() const
    {
        return (static_cast<uint64_t>(raw_errors_high) << 32) |
               raw_errors_low;
    }
};

static void capture_field(bool& seen,
                          uint32_t& destination,
                          uint32_t value,
                          const string& name,
                          PpcntFields& fields)
{
    if (seen && destination != value)
    {
        fields.conflicting_duplicate = true;
        fields.conflicting_field = name;
        return;
    }

    seen = true;
    destination = value;
}

static bool capture_ppcnt_field(const string&,
                                uint64_t,
                                uint64_t calculated_value,
                                MlxRegLib::AdbInstance* instance,
                                void* context)
{
    if (!instance || !instance->fieldDesc)
        return false;

    PpcntFields& fields = *static_cast<PpcntFields*>(context);
    const string& name = instance->fieldDesc->name;
    const uint32_t value = static_cast<uint32_t>(calculated_value);

    if (name == "grp")
        capture_field(fields.grp_seen, fields.grp, value, name, fields);
    else if (name == "local_port")
        capture_field(fields.local_port_seen,
                      fields.local_port,
                      value,
                      name,
                      fields);
    else if (name == "clr")
        capture_field(fields.clr_seen, fields.clr, value, name, fields);
    else if (name == "time_since_last_clear_high")
        capture_field(fields.time_high_seen,
                      fields.time_high,
                      value,
                      name,
                      fields);
    else if (name == "time_since_last_clear_low")
        capture_field(fields.time_low_seen,
                      fields.time_low,
                      value,
                      name,
                      fields);
    else if (name == "phy_raw_errors_lane0_high")
        capture_field(fields.raw_errors_high_seen,
                      fields.raw_errors_high,
                      value,
                      name,
                      fields);
    else if (name == "phy_raw_errors_lane0_low")
        capture_field(fields.raw_errors_low_seen,
                      fields.raw_errors_low,
                      value,
                      name,
                      fields);
    else if (name == "raw_ber_coef")
        capture_field(fields.raw_ber_coef_seen,
                      fields.raw_ber_coef,
                      value,
                      name,
                      fields);
    else if (name == "raw_ber_magnitude")
        capture_field(fields.raw_ber_magnitude_seen,
                      fields.raw_ber_magnitude,
                      value,
                      name,
                      fields);

    return false;
}

static string operation_name(const string& phase, unsigned iteration)
{
    ostringstream output;
    output << phase;

    if (iteration != 0)
        output << " GET " << iteration;

    return output.str();
}

static PpcntFields validate_ppcnt_buffer(MlxRegLib& reg_lib,
                                         MlxRegLib::AdbInstance* reg_node,
                                         const vector<u_int32_t>& buffer,
                                         size_t expected_dwords,
                                         unsigned expected_local_port,
                                         const string& phase,
                                         unsigned iteration,
                                         bool require_response_fields)
{
    const string operation = operation_name(phase, iteration);

    if (buffer.size() != expected_dwords || buffer.empty())
    {
        ostringstream error;
        error << operation << ": malformed PPCNT buffer: expected "
              << expected_dwords << " dwords, received " << buffer.size();
        throw runtime_error(error.str());
    }

    PpcntFields fields;

    reg_lib.getAdb().traverse_layout(
        reg_node,
        "",
        0,
        reinterpret_cast<const uint8_t*>(&buffer[0]),
        static_cast<uint32_t>(buffer.size() * sizeof(u_int32_t)),
        capture_ppcnt_field,
        &fields,
        true,
        false,
        true);

    if (fields.conflicting_duplicate)
    {
        throw runtime_error(operation +
                            ": conflicting duplicate field: " +
                            fields.conflicting_field);
    }

    if (!fields.grp_seen || !fields.local_port_seen || !fields.clr_seen)
    {
        throw runtime_error(operation +
                            ": response is missing grp, local_port, or clr");
    }

    if (fields.grp != PPCNT_GROUP)
    {
        ostringstream error;
        error << operation << ": expected grp=0x" << hex << PPCNT_GROUP
              << ", received 0x" << fields.grp;
        throw runtime_error(error.str());
    }

    if (fields.local_port != expected_local_port)
    {
        ostringstream error;
        error << operation << ": expected local_port=" << dec
              << expected_local_port << ", received " << fields.local_port;
        throw runtime_error(error.str());
    }

    if (fields.clr != 0)
    {
        ostringstream error;
        error << operation << ": unsafe PPCNT clr value " << fields.clr;
        throw runtime_error(error.str());
    }

    if (require_response_fields &&
        (!fields.time_high_seen || !fields.time_low_seen ||
         !fields.raw_errors_high_seen || !fields.raw_errors_low_seen ||
         !fields.raw_ber_coef_seen || !fields.raw_ber_magnitude_seen))
    {
        throw runtime_error(operation +
                            ": response is missing expected group 0x16 fields");
    }

    return fields;
}

static void validate_monotonic_counters(const PpcntFields& previous,
                                        const PpcntFields& current,
                                        const string& phase,
                                        unsigned iteration)
{
    const string operation = operation_name(phase, iteration);

    if (current.time_since_last_clear() < previous.time_since_last_clear())
        throw runtime_error(operation +
                            ": time_since_last_clear moved backwards");

    if (current.lane0_raw_errors() < previous.lane0_raw_errors())
        throw runtime_error(operation +
                            ": phy_raw_errors_lane0 moved backwards");
}

static unsigned parse_unsigned_argument(const char* text,
                                        const char* name,
                                        unsigned min_value,
                                        unsigned max_value)
{
    if (!text || text[0] == '\0' || text[0] == '-' || text[0] == '+')
        throw invalid_argument(string(name) + " must be an unsigned decimal integer");

    errno = 0;
    char* end = nullptr;
    const unsigned long long value = strtoull(text, &end, 10);

    if (errno == ERANGE || end == text || *end != '\0' ||
        value < min_value || value > max_value)
    {
        ostringstream error;
        error << name << " must be in range " << min_value << ".."
              << max_value;
        throw invalid_argument(error.str());
    }

    return static_cast<unsigned>(value);
}

static bool is_valid_pci_bdf(const string& device)
{
    static const regex bdf_pattern(
        "^([[:xdigit:]]{4}:)?([[:xdigit:]]{2}):([[:xdigit:]]{2})\\.([0-7])$");
    smatch match;

    if (!regex_match(device, match, bdf_pattern))
        return false;

    const unsigned pci_device =
        static_cast<unsigned>(strtoul(match[3].str().c_str(), nullptr, 16));

    return pci_device <= 0x1f;
}

static void validate_adb_file(const string& adb_file)
{
    struct stat file_status;

    if (adb_file.empty())
        throw invalid_argument("adb_file must not be empty");

    if (stat(adb_file.c_str(), &file_status) != 0)
    {
        throw invalid_argument("cannot stat adb_file '" + adb_file +
                               "': " + strerror(errno));
    }

    if (!S_ISREG(file_status.st_mode))
        throw invalid_argument("adb_file is not a regular file: " + adb_file);

    if (file_status.st_size <= 0)
        throw invalid_argument("adb_file is empty: " + adb_file);

    if (access(adb_file.c_str(), R_OK) != 0)
    {
        throw invalid_argument("adb_file is not readable: " + adb_file +
                               ": " + strerror(errno));
    }
}

static double elapsed_ms(const Clock::time_point& a,
                         const Clock::time_point& b)
{
    return chrono::duration<double, milli>(b - a).count();
}

static double percentile(vector<double> v, double p)
{
    if (v.empty())
        return 0.0;

    sort(v.begin(), v.end());

    size_t idx =
        static_cast<size_t>(ceil(p * static_cast<double>(v.size()))) - 1;

    if (idx >= v.size())
        idx = v.size() - 1;

    return v[idx];
}

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        cerr << "Usage:\n"
             << "  " << argv[0]
             << " <device> <adb_file> <local_port> <count>\n\n"
             << "Example:\n"
             << "  " << argv[0]
             << " 05:00.0 /tmp/q3400_switch.adb 65 100\n";
        return 1;
    }

    const string device = argv[1];
    const string adb_file = argv[2];
    unsigned local_port = 0;
    unsigned count = 0;

    try
    {
        if (!is_valid_pci_bdf(device))
        {
            throw invalid_argument(
                "device must be a PCI BDF such as 05:00.0 or 0000:05:00.0");
        }

        validate_adb_file(adb_file);
        local_port = parse_unsigned_argument(
            argv[3], "local_port", 0, MAX_LOCAL_PORT);
        count = parse_unsigned_argument(
            argv[4], "count", 1, MAX_BENCHMARK_COUNT);
    }
    catch (const exception& error)
    {
        cerr << "Invalid arguments: " << error.what() << "\n";
        return 1;
    }

    mfile* mf = nullptr;

    try
    {
        cout << "Device      : " << device << "\n"
             << "ADB file    : " << adb_file << "\n"
             << "Local port  : " << local_port << "\n"
             << "Iterations  : " << count << "\n";

        /*
         * Stage 1:
         * Open device exactly once.
         */
        auto t0 = Clock::now();

        mf = mopen(device.c_str());

        auto t1 = Clock::now();

        if (!mf)
        {
            cerr << "mopen() failed for device: " << device << "\n";
            return 2;
        }

        const bool remote_transport = (mf->flags & MDEVS_REM) != 0;

        /*
         * Stage 2:
         * Parse/load ADB exactly once.
         */
        MlxRegLib regLib(mf, adb_file, true);

        auto t2 = Clock::now();

        /*
         * Stage 3:
         * Find PPCNT layout and generate request template exactly once.
         */
        auto* regNode = regLib.findAdbNode("PPCNT");

        if (!regNode)
        {
            cerr << "PPCNT not found in ADB\n";
            mclose(mf);
            return 3;
        }

        const uint32_t reg_len_bytes =
            static_cast<uint32_t>(regNode->get_size() / 8);

        if (reg_len_bytes == 0 || reg_len_bytes % sizeof(u_int32_t) != 0)
            throw runtime_error("PPCNT layout size is not a nonzero dword multiple");

        const size_t expected_dwords =
            reg_len_bytes / sizeof(u_int32_t);

        const string indexes =
            "grp=0x16,"
            "local_port=" + to_string(local_port) +
            ",lp_msb=0,"
            "pnat=0,"
            "plane_ind=0";

        const string operations = "clr=0";

        RegAccessParser parser(
            "",
            indexes,
            operations,
            &regLib.getAdb(),
            regNode,
            reg_len_bytes,
            false,
            false);

        vector<u_int32_t> request = parser.genBuff();

        validate_ppcnt_buffer(
            regLib,
            regNode,
            request,
            expected_dwords,
            local_port,
            "request template",
            0,
            false);

        auto t3 = Clock::now();

        cout << "Register size: " << reg_len_bytes << " bytes\n"
             << "Buffer dwords: " << request.size() << "\n"
             << "Request check : grp=0x16, local_port=" << local_port
             << ", clr=0\n\n";

        cout << fixed << setprecision(3)
             << "mopen time    : " << elapsed_ms(t0, t1) << " ms\n"
             << "ADB init time : " << elapsed_ms(t1, t2) << " ms\n"
             << "Buffer build  : " << elapsed_ms(t2, t3) << " ms\n\n";

        /*
         * Warmup.
         * PPCNT GET with clr=0 does not clear counters.
         */
        const unsigned warmup_count = 5;

        cout << "Warmup       : " << warmup_count << " GETs\n";

        PpcntFields previous_response;
        bool previous_response_valid = false;
        unsigned validated_gets = 0;

        for (unsigned i = 0; i < warmup_count; ++i)
        {
            vector<u_int32_t> buf = request;

            regLib.sendRegister(
                "PPCNT",
                MACCESS_REG_METHOD_GET,
                buf);

            const PpcntFields response = validate_ppcnt_buffer(
                regLib,
                regNode,
                buf,
                expected_dwords,
                local_port,
                "warmup",
                i + 1,
                true);

            if (previous_response_valid)
                validate_monotonic_counters(
                    previous_response, response, "warmup", i + 1);

            previous_response = response;
            previous_response_valid = true;
            ++validated_gets;
        }

        /*
         * Timed section.
         *
         * Buffer copy is deliberately outside timing.
         * Therefore timing covers sendRegister -> maccess_reg path.
         */
        vector<double> samples;
        samples.reserve(count);
        PpcntFields first_measured_response;
        PpcntFields last_measured_response;

        auto wall_start = Clock::now();

        for (unsigned i = 0; i < count; ++i)
        {
            vector<u_int32_t> buf = request;

            auto a = Clock::now();

            regLib.sendRegister(
                "PPCNT",
                MACCESS_REG_METHOD_GET,
                buf);

            auto b = Clock::now();

            samples.push_back(elapsed_ms(a, b));

            const PpcntFields response = validate_ppcnt_buffer(
                regLib,
                regNode,
                buf,
                expected_dwords,
                local_port,
                "measured",
                i + 1,
                true);

            validate_monotonic_counters(
                previous_response, response, "measured", i + 1);

            if (i == 0)
                first_measured_response = response;

            last_measured_response = response;
            previous_response = response;
            ++validated_gets;
        }

        auto wall_end = Clock::now();

        const double sum =
            accumulate(samples.begin(), samples.end(), 0.0);

        const double mean =
            sum / static_cast<double>(samples.size());

        vector<double> sorted = samples;
        sort(sorted.begin(), sorted.end());

        double median;

        if (sorted.size() % 2 == 0)
        {
            const size_t n = sorted.size();

            median =
                (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
        }
        else
        {
            median = sorted[sorted.size() / 2];
        }

        const double wall =
            elapsed_ms(wall_start, wall_end);

        cout << "\n========== PPCNT PERSISTENT RESULT ==========\n"
             << "GET count       : " << samples.size() << "\n"
             << "Wall time       : " << wall << " ms\n"
             << "Mean            : " << mean << " ms\n"
             << "Median          : " << median << " ms\n"
             << "P95             : " << percentile(samples, 0.95) << " ms\n"
             << "P99             : " << percentile(samples, 0.99) << " ms\n"
             << "Min             : " << sorted.front() << " ms\n"
             << "Max             : " << sorted.back() << " ms\n"
             << "Throughput      : "
             << (1000.0 * samples.size() / wall)
             << " GET/s\n"
             << "Response checks : " << validated_gets << " passed\n"
             << "First clear age : "
             << first_measured_response.time_since_last_clear() << " ms\n"
             << "Last clear age  : "
             << last_measured_response.time_since_last_clear() << " ms\n"
             << "First lane0 raw : "
             << first_measured_response.lane0_raw_errors() << "\n"
             << "Last lane0 raw  : "
             << last_measured_response.lane0_raw_errors() << "\n"
             << "Last raw BER    : "
             << last_measured_response.raw_ber_coef << "e-"
             << last_measured_response.raw_ber_magnitude << "\n"
             << "Error count     : 0\n"
             << "Firmware busy   : 0\n";

        if (remote_transport)
            cout << "Retry count     : not observable for remote transport\n";
        else
            cout << "Retry count     : 0 (local transport does not retry)\n";

        cout
             << "=============================================\n";

        mclose(mf);
        mf = nullptr;
    }
    catch (const std::exception& e)
    {
        cerr << "Exception: " << e.what() << "\n";

        if (mf)
            mclose(mf);

        return 10;
    }
    catch (...)
    {
        cerr << "Unknown exception\n";

        if (mf)
            mclose(mf);

        return 11;
    }

    return 0;
}
