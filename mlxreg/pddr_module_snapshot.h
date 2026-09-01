#ifndef Q3400_PDDR_MODULE_SNAPSHOT_H
#define Q3400_PDDR_MODULE_SNAPSHOT_H

#include <cstdint>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "mlxreg/mlxreg_lib/mlxreg_lib.h"
#include "mlxreg/mlxreg_lib/mlxreg_parser.h"

namespace q3400
{

class PddrSchemaError : public std::runtime_error
{
public:
    explicit PddrSchemaError(const std::string& message) : std::runtime_error(message) {}
};

inline bool module_context_fallback_allowed(const std::exception& error)
{
    return dynamic_cast<const PddrSchemaError*>(&error) == nullptr;
}

struct ModuleMediaLane
{
    unsigned lane = 0;
    uint32_t datapath_state = 0;
    std::string datapath_state_name;
    bool datapath_state_available = false;
    bool rx_output_valid_available = false;
    bool rx_output_valid_supported = false;
    bool rx_output_valid = false;
    bool rx_power_available = false;
    bool tx_power_available = false;
    bool tx_bias_available = false;
    uint32_t rx_power_uw = 0;
    uint32_t tx_power_uw = 0;
    uint32_t tx_bias_raw = 0;
    double tx_bias_ua = 0.0;
};

struct ModuleSnapshotData
{
    bool present = true;
    std::string vendor;
    std::string part_number;
    std::string serial_number;
    std::string revision;
    uint32_t cable_identifier = 0;
    std::string cable_identifier_name;
    uint32_t cable_type = 0;
    std::string cable_type_name;
    uint32_t cable_technology = 0;
    std::string cable_technology_name;
    uint32_t ethernet_compliance_code = 0;
    uint32_t ext_ethernet_compliance_code = 0;
    uint32_t active_set_host_compliance_code = 0;
    uint32_t active_set_media_compliance_code = 0;
    uint32_t vendor_oui = 0;
    std::string vendor_oui_hex;
    uint32_t cable_length_raw = 0;
    double transfer_distance_m = 0.0;
    uint32_t cable_breakout_raw = 0;
    uint32_t smf_length_raw = 0;
    uint32_t cable_attenuation_5g_raw = 0;
    uint32_t cable_attenuation_7g_raw = 0;
    uint32_t cable_attenuation_12g_raw = 0;
    uint32_t cable_attenuation_25g_raw = 0;
    uint32_t cable_attenuation_53g_raw = 0;
    uint32_t cable_rx_amp_raw = 0;
    uint32_t cable_rx_pre_emphasis_raw = 0;
    uint32_t cable_rx_post_emphasis_raw = 0;
    uint32_t cable_tx_equalization_raw = 0;
    uint64_t manufacturing_date_raw = 0;
    std::string manufacturing_date;
    bool module_state_available = false;
    uint32_t module_state = 0;
    std::string module_state_name;
    bool fw_version_available = false;
    uint32_t fw_version = 0;
    std::string fw_version_string;
    uint32_t memory_map_revision = 0;
    std::string memory_map_revision_display;
    uint32_t memory_map_compliance = 0;
    std::string memory_map_compliance_display;
    bool wavelength_available = false;
    uint32_t wavelength_nm = 0;
    bool wavelength_tolerance_available = false;
    double wavelength_tolerance_nm = 0.0;
    uint32_t cable_power_class = 0;
    uint32_t max_power_raw = 0;
    bool max_power_available = false;
    double max_power_w = 0.0;
    bool digital_diagnostic_monitoring = false;
    uint32_t rx_cdr_cap = 0;
    uint32_t tx_cdr_cap = 0;
    uint32_t rx_cdr_state = 0;
    uint32_t tx_cdr_state = 0;
    uint32_t rx_power_type = 0;
    std::string rx_power_type_name;
    uint32_t ib_width_raw = 0;
    uint32_t nominal_bit_rate_100_raw = 0;
    uint32_t nominal_bit_rate_250_raw = 0;
    bool linear_direct_drive = false;
    bool rx_output_valid_capable = false;
    uint32_t error_code = 0;
    std::string error_code_name;
    uint32_t monitor_cap_mask = 0;
    bool temperature_available = false;
    double temperature_c = 0.0;
    bool temperature_thresholds_available = false;
    double temperature_low_c = 0.0;
    double temperature_high_c = 0.0;
    bool voltage_available = false;
    double voltage_mv = 0.0;
    bool voltage_thresholds_available = false;
    double voltage_low_mv = 0.0;
    double voltage_high_mv = 0.0;
    bool rx_power_thresholds_available = false;
    uint32_t rx_power_low_uw = 0;
    uint32_t rx_power_high_uw = 0;
    bool tx_power_thresholds_available = false;
    uint32_t tx_power_low_uw = 0;
    uint32_t tx_power_high_uw = 0;
    bool tx_bias_thresholds_available = false;
    double tx_bias_low_ua = 0.0;
    double tx_bias_high_ua = 0.0;
    std::vector<ModuleMediaLane> media_lanes;
    std::set<std::string> optional_fields;

    bool has_optional(const std::string& name) const
    {
        return optional_fields.count(name) != 0;
    }
};

class PddrModuleSnapshotReader
{
public:
    typedef mlxreg::MlxRegLib::AdbInstance AdbInstance;
    typedef std::map<std::string, AdbInstance*> FieldMap;

    explicit PddrModuleSnapshotReader(mlxreg::MlxRegLib& reg) : _reg(reg)
    {
        _node = _reg.findAdbNode("PDDR");
        if (!_node)
            throw std::runtime_error("PDDR not found in ADB");
        _bytes = static_cast<uint32_t>(_node->get_size() / 8);
        if (_bytes != 0x100 || _bytes % sizeof(u_int32_t) != 0)
            throw std::runtime_error("unexpected PDDR register size: " +
                                     std::to_string(_bytes));
    }

    std::vector<u_int32_t> build_request(unsigned local_port)
    {
        const std::string indexes =
            "local_port=" + std::to_string(local_port) +
            ",lp_msb=0,pnat=0,plane_ind=0,port_type=0,module_ind_type=0,page_select=3";
        mlxreg::RegAccessParser parser("", indexes, "module_info_ext=1",
                                       &_reg.getAdb(), _node, _bytes, false, false);
        std::vector<u_int32_t> request = parser.genBuff();
        if (request.size() != _bytes / sizeof(u_int32_t))
            throw std::runtime_error("PDDR request buffer size mismatch");
        const FieldMap fields = capture_checked(request, "request template");
        validate_header(fields, request, local_port, "request template");
        return request;
    }

    ModuleSnapshotData read(unsigned local_port,
                            const std::vector<u_int32_t>& request)
    {
        const FieldMap request_fields = capture_checked(request, "request template");
        validate_header(request_fields, request, local_port, "request template");

        std::vector<u_int32_t> response = request;
        _reg.sendRegister("PDDR", MACCESS_REG_METHOD_GET, response);
        if (response.size() != _bytes / sizeof(u_int32_t))
            throw std::runtime_error("PDDR response buffer size mismatch");
        const FieldMap fields = capture_checked(response, "response");
        validate_header(fields, response, local_port, "response");
        try
        {
            return decode(fields, response);
        }
        catch (const PddrSchemaError&)
        {
            throw;
        }
        catch (const std::exception& error)
        {
            throw PddrSchemaError(std::string("PDDR page 3 deterministic decode failure: ") +
                                  error.what());
        }
    }

    uint32_t response_bytes() const { return _bytes; }

    // Pure formatting helpers are public so local fixture tests can verify the
    // exact mlxlink-compatible enum/version materialization without hardware.
    static std::string format_fw_version(uint32_t value)
    {
        return module_fw_version(value);
    }

    static std::string format_module_state(uint32_t value)
    {
        return module_state_name(value);
    }

    static std::string format_datapath_state(uint32_t value)
    {
        return datapath_state_name(value);
    }

private:
    static bool capture_field(const std::string& path,
                              uint64_t,
                              uint64_t,
                              AdbInstance* instance,
                              void* opaque)
    {
        if (!instance || !instance->fieldDesc)
            return false;
        FieldMap& fields = *static_cast<FieldMap*>(opaque);
        fields.insert(std::make_pair(instance->fieldDesc->name, instance));
        fields.insert(std::make_pair(path, instance));
        return false;
    }

    FieldMap capture(const std::vector<u_int32_t>& buffer)
    {
        FieldMap fields;
        _reg.getAdb().traverse_layout(
            _node, "", 0, reinterpret_cast<const uint8_t*>(&buffer[0]), _bytes,
            capture_field, &fields, true, false, true);
        return fields;
    }

    FieldMap capture_checked(const std::vector<u_int32_t>& buffer,
                             const std::string& phase)
    {
        try
        {
            return capture(buffer);
        }
        catch (const std::exception& error)
        {
            throw PddrSchemaError("PDDR page 3 " + phase +
                                  " layout traversal failed: " + error.what());
        }
    }

    static AdbInstance* field(const FieldMap& fields, const std::string& name)
    {
        FieldMap::const_iterator exact = fields.find(name);
        if (exact != fields.end())
            return exact->second;
        const std::string suffix = "." + name;
        for (FieldMap::const_iterator it = fields.begin(); it != fields.end(); ++it)
        {
            if (it->first.size() >= suffix.size() &&
                it->first.compare(it->first.size() - suffix.size(), suffix.size(), suffix) == 0)
                return it->second;
        }
        throw PddrSchemaError("PDDR page 3 layout is missing required field: " + name);
    }

    static uint64_t pop(const FieldMap& fields,
                        const std::string& name,
                        const std::vector<u_int32_t>& buffer)
    {
        return field(fields, name)->popBuf(reinterpret_cast<uint8_t*>(
            const_cast<u_int32_t*>(&buffer[0])));
    }

    static bool try_pop(const FieldMap& fields,
                        const std::string& name,
                        const std::vector<u_int32_t>& buffer,
                        uint64_t& value)
    {
        try
        {
            value = pop(fields, name, buffer);
            return true;
        }
        catch (const PddrSchemaError&)
        {
            return false;
        }
    }

    static bool optional_u32(ModuleSnapshotData& snapshot,
                             const FieldMap& fields,
                             const std::string& name,
                             const std::vector<u_int32_t>& buffer,
                             uint32_t& value)
    {
        uint64_t raw = 0;
        if (!try_pop(fields, name, buffer, raw))
            return false;
        value = static_cast<uint32_t>(raw);
        snapshot.optional_fields.insert(name);
        return true;
    }

    static std::string ascii_bytes(const FieldMap& fields,
                                   const std::string& name,
                                   unsigned bytes,
                                   const std::vector<u_int32_t>& buffer)
    {
        std::string value;
        for (unsigned i = 0; i < bytes / 4; ++i)
        {
            const uint32_t word = static_cast<uint32_t>(
                pop(fields, name + "[" + std::to_string(i) + "]", buffer));
            for (int shift = 24; shift >= 0; shift -= 8)
                value.push_back(static_cast<char>((word >> shift) & 0xff));
        }
        while (!value.empty() && (value.back() == '\0' || value.back() == ' '))
            value.pop_back();
        while (!value.empty() && value.front() == '\0')
            value.erase(value.begin());
        return value;
    }

    static std::string ascii_word(uint32_t word)
    {
        std::string value;
        for (int shift = 24; shift >= 0; shift -= 8)
            value.push_back(static_cast<char>((word >> shift) & 0xff));
        while (!value.empty() && (value.back() == '\0' || value.back() == ' '))
            value.pop_back();
        while (!value.empty() && (value.front() == '\0' || value.front() == ' '))
            value.erase(value.begin());
        return value;
    }

    static bool is_cmis_identifier(uint32_t identifier)
    {
        return identifier == 5 || identifier == 6 || identifier == 7 ||
               identifier == 8 || identifier == 10;
    }

    static std::string module_state_name(uint32_t value)
    {
        static const char* names[] = {
            "", "LowPwr state", "PwrUp state", "Ready state",
            "PwrDn state", "Fault state"
        };
        return value < sizeof(names) / sizeof(names[0]) ? names[value] : "";
    }

    static std::string datapath_state_name(uint32_t value)
    {
        static const char* names[] = {
            "", "DPDeactivated", "DPInit", "DPDeinit", "DPActivated",
            "DPTxTurnOn", "DPTxTurnOff", "DPInitialized"
        };
        return value < sizeof(names) / sizeof(names[0]) ? names[value] : "";
    }

    static std::string cable_identifier_name(uint32_t value)
    {
        static const char* names[] = {
            "QSFP28", "QSFP+", "SFP28/SFP+", "QSA (QSFP->SFP)",
            "Backplane", "SFP-DD", "QSFP-DD", "QSFP_CMIS", "OSFP",
            "", "DSFP"
        };
        return value < sizeof(names) / sizeof(names[0]) ? names[value] : "";
    }

    static std::string cable_type_name(uint32_t value)
    {
        static const char* names[] = {
            "Unidentified", "Active cable (active copper / optics)",
            "Optical Module (separated)", "Passive copper cable",
            "Cable unplugged", "Twisted Pair", "CPO", "OE", "ELS"
        };
        return value < sizeof(names) / sizeof(names[0]) ? names[value] : "";
    }

    static std::string cmis_technology_name(uint32_t value)
    {
        static const char* names[] = {
            "850 nm VCSEL", "1310 nm VCSEL", "1550 nm VCSEL",
            "1310 nm FP laser", "1310 nm DFB laser", "1550 nm DFB laser",
            "1310 nm EML", "1550 nm EML", "Other / Undefined",
            "1490 nm DFB laser", "Copper cable, passive, unequalized",
            "Copper cable, passive, equalized",
            "Copper cable with near and far end limiting active equalizers",
            "Copper cable with far end limiting active equalizers",
            "Copper cable with near end limiting active equalizers",
            "Copper cable with linear active equalizers", "C-band tunable laser",
            "B-band tunable laser",
            "Copper cable with near end and far end linear active equalizers",
            "Copper cable with far end linear active equalizers",
            "Copper cable with near end linear active equalizers"
        };
        return value < sizeof(names) / sizeof(names[0]) ? names[value] : "";
    }

    static std::string module_fw_version(uint32_t value)
    {
        if (!value)
            return "";
        return std::to_string((value >> 24) & 0xff) + "." +
               std::to_string((value >> 16) & 0xff) + "." +
               std::to_string(value & 0xffff);
    }

    static std::string error_code_name(uint32_t value)
    {
        switch (value)
        {
            case 0x0: return "ConfigUndefined";
            case 0x1: return "ConfigSuccess";
            case 0x2: return "ConfigRejected";
            case 0x3: return "ConfigRejectedInvalidAppSel";
            case 0x4: return "ConfigRejectedInvalidDataPath";
            case 0x5: return "ConfigRejectedInvalidSI";
            case 0x6: return "ConfigRejectedLanesInUse";
            case 0x7: return "ConfigRejectedPartialDataPath";
            case 0xc: return "ConfigInProgress";
            case 0xd: return "ConfigRejectedInvalidVS_SI";
            default: return "";
        }
    }

    static std::string hex_oui(uint32_t value)
    {
        std::ostringstream output;
        output << "0x" << std::uppercase << std::hex << value;
        return output.str();
    }

    static double cable_length_m(uint32_t raw, bool cmis)
    {
        if (!cmis)
            return raw;
        static const double multipliers[] = {0.1, 1.0, 10.0, 100.0};
        return (raw & 0x3f) * multipliers[(raw >> 6) & 0x3];
    }

    static std::string date_code_string(uint64_t value)
    {
        uint64_t reversed = 0;
        for (unsigned part = 0; part < 4; ++part)
            reversed = (reversed << 16) | ((value >> (part * 16)) & 0xffff);
        std::string result;
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            const char ch = static_cast<char>((reversed >> shift) & 0xff);
            if (ch)
            {
                result.push_back(ch);
                if (shift % 16 == 0)
                    result.push_back('_');
            }
        }
        while (!result.empty() && (result.back() == '_' || result.back() == ' '))
            result.pop_back();
        return result;
    }

    static void validate_header(const FieldMap& fields,
                                const std::vector<u_int32_t>& buffer,
                                unsigned local_port,
                                const std::string& phase)
    {
        if (pop(fields, "local_port", buffer) != local_port ||
            pop(fields, "page_select", buffer) != 3 ||
            pop(fields, "module_info_ext", buffer) != 1 ||
            pop(fields, "pnat", buffer) != 0)
            throw std::runtime_error("PDDR page 3 " + phase + " header mismatch");
    }

    static ModuleSnapshotData decode(const FieldMap& fields,
                                     const std::vector<u_int32_t>& response)
    {
        ModuleSnapshotData snapshot;
        snapshot.vendor = ascii_bytes(fields, "vendor_name", 16, response);
        snapshot.part_number = ascii_bytes(fields, "vendor_pn", 16, response);
        snapshot.serial_number = ascii_bytes(fields, "vendor_sn", 16, response);
        snapshot.revision = ascii_word(
            static_cast<uint32_t>(pop(fields, "vendor_rev", response)));
        snapshot.cable_identifier =
            static_cast<uint32_t>(pop(fields, "cable_identifier", response));
        snapshot.cable_identifier_name =
            cable_identifier_name(snapshot.cable_identifier);
        snapshot.cable_type =
            static_cast<uint32_t>(pop(fields, "cable_type", response));
        snapshot.cable_type_name = cable_type_name(snapshot.cable_type);
        snapshot.cable_technology =
            static_cast<uint32_t>(pop(fields, "cable_technology", response));
        if (is_cmis_identifier(snapshot.cable_identifier))
            snapshot.cable_technology_name =
                cmis_technology_name(snapshot.cable_technology);
        optional_u32(snapshot, fields, "ethernet_compliance_code", response,
                     snapshot.ethernet_compliance_code);
        optional_u32(snapshot, fields, "ext_ethernet_compliance_code", response,
                     snapshot.ext_ethernet_compliance_code);
        optional_u32(snapshot, fields, "active_set_host_compliance_code", response,
                     snapshot.active_set_host_compliance_code);
        optional_u32(snapshot, fields, "active_set_media_compliance_code", response,
                     snapshot.active_set_media_compliance_code);
        if (optional_u32(snapshot, fields, "vendor_oui", response, snapshot.vendor_oui))
            snapshot.vendor_oui_hex = hex_oui(snapshot.vendor_oui);
        if (optional_u32(snapshot, fields, "cable_length", response,
                         snapshot.cable_length_raw))
            snapshot.transfer_distance_m = cable_length_m(
                snapshot.cable_length_raw, is_cmis_identifier(snapshot.cable_identifier));
        optional_u32(snapshot, fields, "cable_breakout", response,
                     snapshot.cable_breakout_raw);
        optional_u32(snapshot, fields, "smf_length", response, snapshot.smf_length_raw);
        optional_u32(snapshot, fields, "cable_attenuation_5g", response,
                     snapshot.cable_attenuation_5g_raw);
        optional_u32(snapshot, fields, "cable_attenuation_7g", response,
                     snapshot.cable_attenuation_7g_raw);
        optional_u32(snapshot, fields, "cable_attenuation_12g", response,
                     snapshot.cable_attenuation_12g_raw);
        optional_u32(snapshot, fields, "cable_attenuation_25g", response,
                     snapshot.cable_attenuation_25g_raw);
        optional_u32(snapshot, fields, "cable_attenuation_53g", response,
                     snapshot.cable_attenuation_53g_raw);
        optional_u32(snapshot, fields, "cable_rx_amp", response,
                     snapshot.cable_rx_amp_raw);
        optional_u32(snapshot, fields, "cable_rx_emphasis", response,
                     snapshot.cable_rx_pre_emphasis_raw);
        optional_u32(snapshot, fields, "cable_rx_post_emphasis", response,
                     snapshot.cable_rx_post_emphasis_raw);
        optional_u32(snapshot, fields, "cable_tx_equalization", response,
                     snapshot.cable_tx_equalization_raw);

        // The ADB contains date_code as a uint64 subnode with children hi/lo.
        // mlxlink's getFieldValue() exposes flattened aliases date_code_hi/lo,
        // while ADB traversal exposes the real paths date_code.hi/.lo.
        uint64_t date_hi = 0;
        uint64_t date_lo = 0;
        if (try_pop(fields, "date_code.hi", response, date_hi) &&
            try_pop(fields, "date_code.lo", response, date_lo))
        {
            snapshot.manufacturing_date_raw = (date_hi << 32) | date_lo;
            snapshot.manufacturing_date = date_code_string(snapshot.manufacturing_date_raw);
            snapshot.optional_fields.insert("date_code");
        }
        snapshot.present = snapshot.cable_type != 4;
        snapshot.module_state =
            static_cast<uint32_t>(pop(fields, "module_st", response));
        snapshot.module_state_available = is_cmis_identifier(snapshot.cable_identifier);
        if (snapshot.module_state_available)
            snapshot.module_state_name = module_state_name(snapshot.module_state);
        snapshot.fw_version =
            static_cast<uint32_t>(pop(fields, "fw_version", response));
        snapshot.fw_version_available = snapshot.fw_version != 0;
        snapshot.fw_version_string = module_fw_version(snapshot.fw_version);
        snapshot.memory_map_revision =
            static_cast<uint32_t>(pop(fields, "memory_map_rev", response));
        snapshot.memory_map_revision_display =
            std::to_string(snapshot.memory_map_revision);
        if (optional_u32(snapshot, fields, "memory_map_compliance", response,
                         snapshot.memory_map_compliance))
            snapshot.memory_map_compliance_display =
                ascii_word(snapshot.memory_map_compliance);
        snapshot.wavelength_nm =
            static_cast<uint32_t>(pop(fields, "wavelength", response));
        snapshot.wavelength_available = snapshot.wavelength_nm != 0;
        uint32_t wavelength_tolerance_raw = 0;
        if (optional_u32(snapshot, fields, "wavelength_tolerance", response,
                         wavelength_tolerance_raw))
        {
            snapshot.wavelength_tolerance_available =
                snapshot.present && snapshot.cable_type != 3 &&
                wavelength_tolerance_raw != 0;
            snapshot.wavelength_tolerance_nm = wavelength_tolerance_raw / 200.0;
        }
        optional_u32(snapshot, fields, "cable_power_class", response,
                     snapshot.cable_power_class);
        if (optional_u32(snapshot, fields, "max_power", response,
                         snapshot.max_power_raw))
        {
            snapshot.max_power_available =
                snapshot.present && snapshot.cable_type != 3 &&
                snapshot.max_power_raw != 0;
            snapshot.max_power_w = snapshot.max_power_raw * 0.25;
        }
        optional_u32(snapshot, fields, "rx_cdr_cap", response, snapshot.rx_cdr_cap);
        optional_u32(snapshot, fields, "tx_cdr_cap", response, snapshot.tx_cdr_cap);
        optional_u32(snapshot, fields, "rx_cdr_state", response, snapshot.rx_cdr_state);
        optional_u32(snapshot, fields, "tx_cdr_state", response, snapshot.tx_cdr_state);
        if (optional_u32(snapshot, fields, "rx_power_type", response,
                         snapshot.rx_power_type))
            snapshot.rx_power_type_name =
                snapshot.rx_power_type == 0 ? "OMA" : "Average power";
        optional_u32(snapshot, fields, "ib_width", response, snapshot.ib_width_raw);
        optional_u32(snapshot, fields, "nbr100", response,
                     snapshot.nominal_bit_rate_100_raw);
        optional_u32(snapshot, fields, "nbr250", response,
                     snapshot.nominal_bit_rate_250_raw);
        uint32_t did_cap = 0;
        if (optional_u32(snapshot, fields, "did_cap", response, did_cap))
            snapshot.linear_direct_drive = did_cap != 0;
        if (optional_u32(snapshot, fields, "error_code", response,
                         snapshot.error_code))
            snapshot.error_code_name = error_code_name(snapshot.error_code);
        optional_u32(snapshot, fields, "monitor_cap_mask", response,
                     snapshot.monitor_cap_mask);
        snapshot.digital_diagnostic_monitoring =
            snapshot.has_optional("monitor_cap_mask") && snapshot.monitor_cap_mask != 0;

        snapshot.temperature_available = (snapshot.monitor_cap_mask & (1u << 0)) != 0;
        const int16_t temperature_raw = static_cast<int16_t>(
            static_cast<uint16_t>(pop(fields, "temperature", response)));
        snapshot.temperature_c = temperature_raw / 256.0;
        uint64_t temperature_low_value = 0;
        uint64_t temperature_high_value = 0;
        if (try_pop(fields, "temperature_low_th", response, temperature_low_value) &&
            try_pop(fields, "temperature_high_th", response, temperature_high_value))
        {
            snapshot.optional_fields.insert("temperature_low_th");
            snapshot.optional_fields.insert("temperature_high_th");
            snapshot.temperature_thresholds_available = snapshot.temperature_available;
            const int16_t temperature_low_raw = static_cast<int16_t>(
                static_cast<uint16_t>(temperature_low_value));
            const int16_t temperature_high_raw = static_cast<int16_t>(
                static_cast<uint16_t>(temperature_high_value));
            snapshot.temperature_low_c = temperature_low_raw / 256.0;
            snapshot.temperature_high_c = temperature_high_raw / 256.0;
        }
        snapshot.voltage_available = (snapshot.monitor_cap_mask & (1u << 1)) != 0;
        snapshot.voltage_mv = pop(fields, "voltage", response) / 10.0;
        uint64_t voltage_low_value = 0;
        uint64_t voltage_high_value = 0;
        if (try_pop(fields, "voltage_low_th", response, voltage_low_value) &&
            try_pop(fields, "voltage_high_th", response, voltage_high_value))
        {
            snapshot.optional_fields.insert("voltage_low_th");
            snapshot.optional_fields.insert("voltage_high_th");
            snapshot.voltage_thresholds_available = snapshot.voltage_available;
            snapshot.voltage_low_mv = voltage_low_value / 10.0;
            snapshot.voltage_high_mv = voltage_high_value / 10.0;
        }

        const bool tx_power_available = (snapshot.monitor_cap_mask & (1u << 2)) != 0;
        const bool rx_power_available = (snapshot.monitor_cap_mask & (1u << 3)) != 0;
        const bool tx_bias_available = (snapshot.monitor_cap_mask & (1u << 4)) != 0;
        uint64_t rx_valid_cap_value = 0;
        const bool rx_valid_cap_available =
            try_pop(fields, "rx_output_valid_cap", response, rx_valid_cap_value);
        if (rx_valid_cap_available)
            snapshot.optional_fields.insert("rx_output_valid_cap");
        const bool rx_valid_supported = rx_valid_cap_value != 0;
        snapshot.rx_output_valid_capable = rx_valid_supported;
        uint64_t rx_valid_value = 0;
        const bool rx_valid_available =
            try_pop(fields, "rx_output_valid", response, rx_valid_value);
        if (rx_valid_available)
            snapshot.optional_fields.insert("rx_output_valid");
        const uint32_t rx_valid_mask = static_cast<uint32_t>(rx_valid_value);
        uint64_t bias_scale_value = 0;
        if (try_pop(fields, "tx_bias_scaling_factor", response, bias_scale_value))
            snapshot.optional_fields.insert("tx_bias_scaling_factor");
        const uint32_t bias_scale = static_cast<uint32_t>(bias_scale_value);
        const uint32_t bias_multiplier = bias_scale <= 2 ? (1u << bias_scale) : 1u;
        uint32_t rx_power_low = 0;
        uint32_t rx_power_high = 0;
        if (optional_u32(snapshot, fields, "rx_power_low_th", response, rx_power_low) &&
            optional_u32(snapshot, fields, "rx_power_high_th", response, rx_power_high))
        {
            snapshot.rx_power_thresholds_available = rx_power_available;
            snapshot.rx_power_low_uw = rx_power_low;
            snapshot.rx_power_high_uw = rx_power_high;
        }
        uint32_t tx_power_low = 0;
        uint32_t tx_power_high = 0;
        if (optional_u32(snapshot, fields, "tx_power_low_th", response, tx_power_low) &&
            optional_u32(snapshot, fields, "tx_power_high_th", response, tx_power_high))
        {
            snapshot.tx_power_thresholds_available = tx_power_available;
            snapshot.tx_power_low_uw = tx_power_low;
            snapshot.tx_power_high_uw = tx_power_high;
        }
        uint64_t tx_bias_low = 0;
        uint64_t tx_bias_high = 0;
        if (try_pop(fields, "tx_bias_low_th", response, tx_bias_low) &&
            try_pop(fields, "tx_bias_high_th", response, tx_bias_high))
        {
            snapshot.optional_fields.insert("tx_bias_low_th");
            snapshot.optional_fields.insert("tx_bias_high_th");
            snapshot.tx_bias_thresholds_available = tx_bias_available;
            snapshot.tx_bias_low_ua = tx_bias_low * 2.0 * bias_multiplier;
            snapshot.tx_bias_high_ua = tx_bias_high * 2.0 * bias_multiplier;
        }

        // Page 3 defines eight media-lane slots.  Their state/capability fields,
        // not a non-zero DDM value, tell consumers whether a slot is meaningful.
        for (unsigned lane = 0; lane < 8; ++lane)
        {
            const std::string suffix = std::to_string(lane);
            ModuleMediaLane item;
            item.lane = lane;
            item.datapath_state = static_cast<uint32_t>(
                pop(fields, "dp_st_lane[" + suffix + "]", response));
            item.datapath_state_available =
                item.datapath_state >= 1 && item.datapath_state <= 7;
            if (item.datapath_state_available)
                item.datapath_state_name = datapath_state_name(item.datapath_state);
            // mlxlink -m materializes the PDDR3 bitmask directly.  Preserve the
            // advertised capability separately instead of using it to hide a
            // valid false (0) lane status.
            item.rx_output_valid_available = snapshot.present && rx_valid_available;
            item.rx_output_valid_supported = rx_valid_supported;
            item.rx_output_valid = (rx_valid_mask & (1u << lane)) != 0;
            item.rx_power_available = rx_power_available;
            item.tx_power_available = tx_power_available;
            item.tx_bias_available = tx_bias_available;
            item.rx_power_uw = static_cast<uint32_t>(
                pop(fields, "rx_power_lane" + suffix, response));
            item.tx_power_uw = static_cast<uint32_t>(
                pop(fields, "tx_power_lane" + suffix, response));
            item.tx_bias_raw = static_cast<uint32_t>(
                pop(fields, "tx_bias_lane" + suffix, response));
            item.tx_bias_ua = item.tx_bias_raw * 2.0 * bias_multiplier;
            snapshot.media_lanes.push_back(item);
        }
        return snapshot;
    }

    mlxreg::MlxRegLib& _reg;
    AdbInstance* _node = nullptr;
    uint32_t _bytes = 0;
};

} // namespace q3400

#endif
