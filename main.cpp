#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iphlpapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#pragma comment(lib, "iphlpapi.lib")

// -----------------------------------------------------------------------------
// Global counters consumed by ledger.asm
// -----------------------------------------------------------------------------

extern "C" {

alignas(64) volatile std::uint64_t g_inboundBytes = 0;
alignas(64) volatile std::uint64_t g_outboundBytes = 0;
alignas(64) volatile std::uint64_t g_inboundPackets = 0;
alignas(64) volatile std::uint64_t g_outboundPackets = 0;

void UpdateTrafficCounters(
    std::uint64_t inboundBytes,
    std::uint64_t outboundBytes,
    std::uint64_t inboundPackets,
    std::uint64_t outboundPackets
);

}

// -----------------------------------------------------------------------------
// Snapshot
// -----------------------------------------------------------------------------

struct TrafficSnapshot
{
    std::uint64_t inboundBytes = 0;
    std::uint64_t outboundBytes = 0;
    std::uint64_t inboundPackets = 0;
    std::uint64_t outboundPackets = 0;
};

// -----------------------------------------------------------------------------
// Read Windows interface counters
//
// GetIfTable2() provides cumulative interface statistics maintained by Windows.
// We aggregate all interfaces belonging to this terminal.
// -----------------------------------------------------------------------------

bool ReadInterfaceCounters(TrafficSnapshot& result)
{
    MIB_IF_TABLE2* table = nullptr;

    DWORD status = GetIfTable2(&table);

    if (status != NO_ERROR)
    {
        std::cerr
            << "GetIfTable2 failed. Error: "
            << status
            << '\n';

        return false;
    }

    for (ULONG i = 0; i < table->NumEntries; ++i)
    {
        const MIB_IF_ROW2& row = table->Table[i];

        result.inboundBytes += row.InOctets;
        result.outboundBytes += row.OutOctets;

        result.inboundPackets += row.InUcastPkts;
        result.outboundPackets += row.OutUcastPkts;
    }

    FreeMibTable(table);

    return true;
}

// -----------------------------------------------------------------------------
// UTC timestamp
// -----------------------------------------------------------------------------

std::string TimestampUTC()
{
    SYSTEMTIME st{};
    GetSystemTime(&st);

    std::ostringstream out;

    out << std::setfill('0')
        << std::setw(4) << st.wYear << '-'
        << std::setw(2) << st.wMonth << '-'
        << std::setw(2) << st.wDay << 'T'
        << std::setw(2) << st.wHour << ':'
        << std::setw(2) << st.wMinute << ':'
        << std::setw(2) << st.wSecond << '.'
        << std::setw(3) << st.wMilliseconds
        << 'Z';

    return out.str();
}

// -----------------------------------------------------------------------------
// Append one ledger record
// -----------------------------------------------------------------------------

void AppendLedgerRecord(
    std::ofstream& ledger,
    std::uint64_t sequence)
{
    const auto inboundBytes =
        g_inboundBytes;

    const auto outboundBytes =
        g_outboundBytes;

    const auto inboundPackets =
        g_inboundPackets;

    const auto outboundPackets =
        g_outboundPackets;

    ledger
        << sequence << ','
        << TimestampUTC() << ','
        << inboundBytes << ','
        << outboundBytes << ','
        << inboundPackets << ','
        << outboundPackets
        << '\n';

    ledger.flush();
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main()
{
    constexpr auto SAMPLE_INTERVAL =
        std::chrono::seconds(60);

    const std::filesystem::path ledgerPath =
        "traffic_ledger.csv";

    // -------------------------------------------------------------------------
    // Open/create ledger
    // -------------------------------------------------------------------------

    const bool ledgerAlreadyExists =
        std::filesystem::exists(ledgerPath);

    std::ofstream ledger(
        ledgerPath,
        std::ios::app
    );

    if (!ledger)
    {
        std::cerr
            << "Unable to open ledger: "
            << ledgerPath
            << '\n';

        return 1;
    }

    if (!ledgerAlreadyExists ||
        std::filesystem::file_size(ledgerPath) == 0)
    {
        ledger
            << "sequence,"
            << "timestamp_utc,"
            << "inbound_bytes,"
            << "outbound_bytes,"
            << "inbound_packets,"
            << "outbound_packets"
            << '\n';

        ledger.flush();
    }

    // -------------------------------------------------------------------------
    // Establish baseline
    //
    // Windows' interface counters are already cumulative. We calculate the
    // difference between successive observations and give that difference
    // to the MASM accumulator.
    // -------------------------------------------------------------------------

    TrafficSnapshot previous;

    if (!ReadInterfaceCounters(previous))
    {
        return 1;
    }

    std::uint64_t sequence = 0;

    std::cout
        << "Traffic ledger started.\n"
        << "Ledger: "
        << ledgerPath
        << '\n'
        << "Sampling interval: "
        << SAMPLE_INTERVAL.count()
        << " seconds\n";

    // -------------------------------------------------------------------------
    // Continuous accounting loop
    // -----------------------------------------------------------------------------

    for (;;)
    {
        std::this_thread::sleep_for(SAMPLE_INTERVAL);

        TrafficSnapshot current;

        if (!ReadInterfaceCounters(current))
        {
            std::cerr
                << "Unable to read interface counters; "
                << "retaining previous baseline.\n";

            continue;
        }

        // ---------------------------------------------------------------------
        // Normally Windows interface counters monotonically increase.
        //
        // If an interface resets/reinitializes, its counter can decrease.
        // In that case we treat the current value as the new baseline rather
        // than producing a huge unsigned underflow.
        // ---------------------------------------------------------------------

        const auto delta = [](
            std::uint64_t currentValue,
            std::uint64_t previousValue)
            -> std::uint64_t
        {
            if (currentValue >= previousValue)
                return currentValue - previousValue;

            return currentValue;
        };

        const std::uint64_t inboundBytes =
            delta(
                current.inboundBytes,
                previous.inboundBytes
            );

        const std::uint64_t outboundBytes =
            delta(
                current.outboundBytes,
                previous.outboundBytes
            );

        const std::uint64_t inboundPackets =
            delta(
                current.inboundPackets,
                previous.inboundPackets
            );

        const std::uint64_t outboundPackets =
            delta(
                current.outboundPackets,
                previous.outboundPackets
            );

        // ---------------------------------------------------------------------
        // Send the interval deltas to the ASM accounting primitive.
        // ---------------------------------------------------------------------

        UpdateTrafficCounters(
            inboundBytes,
            outboundBytes,
            inboundPackets,
            outboundPackets
        );

        previous = current;

        ++sequence;

        // ---------------------------------------------------------------------
        // Write cumulative state to the ledger.
        // ---------------------------------------------------------------------

        AppendLedgerRecord(
            ledger,
            sequence
        );

        std::cout
            << TimestampUTC()
            << " | IN "
            << inboundBytes
            << " bytes | OUT "
            << outboundBytes
            << " bytes\n";
    }

    return 0;
}