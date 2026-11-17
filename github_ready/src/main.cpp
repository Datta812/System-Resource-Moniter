
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <iomanip>
#include <stdexcept>
#include <map>
#include <sqlite3.h>
#include <csignal>
#include <atomic>
#include <cctype>
#include <sys/stat.h>

using namespace std;

// Set by SIGINT/SIGTERM so the loop can exit cleanly (database is closed properly).
static atomic<bool> keepRunning{true};
static void handleSignal(int) { keepRunning = false; }

// Keep only the newest N rows so the database does not grow forever (~24 h at 1 Hz).
static const int MAX_ROWS = 86400;

// ==================== CPU MONITORING ====================

struct CPUStats {
    unsigned long long user = 0, nice = 0, system = 0;
    unsigned long long idle = 0, iowait = 0, irq = 0;
    unsigned long long softirq = 0, steal = 0;
};

CPUStats readCPUStats() {
    ifstream file("/proc/stat");
    string label;
    CPUStats s;

    if (!file ||
        !(file >> label >> s.user >> s.nice >> s.system
               >> s.idle >> s.iowait >> s.irq
               >> s.softirq >> s.steal)) {
        throw runtime_error("Unable to read /proc/stat");
    }
    return s;
}

unsigned long long totalCPUTime(const CPUStats& s) {
    return s.user + s.nice + s.system + s.idle
         + s.iowait + s.irq + s.softirq + s.steal;
}

double calculateCPUUsage(const CPUStats& oldS,
                         const CPUStats& newS) {
    auto oldTotal = totalCPUTime(oldS);
    auto newTotal = totalCPUTime(newS);
    auto oldIdle = oldS.idle + oldS.iowait;
    auto newIdle = newS.idle + newS.iowait;

    if (newTotal <= oldTotal || newIdle < oldIdle)
        return 0.0;

    auto totalDelta = newTotal - oldTotal;
    auto idleDelta = newIdle - oldIdle;

    if (totalDelta == 0 || idleDelta > totalDelta)
        return 0.0;

    return 100.0 * (totalDelta - idleDelta) / totalDelta;
}

// ==================== MEMORY MONITORING ====================

struct MemoryStats {
    unsigned long long totalKB = 0;
    unsigned long long availableKB = 0;
    unsigned long long usedKB = 0;
};

MemoryStats readMemoryStats() {
    ifstream file("/proc/meminfo");

    if (!file)
        throw runtime_error("Unable to open /proc/meminfo");

    MemoryStats memory;
    string key, unit;
    unsigned long long value;

    while (file >> key >> value >> unit) {
        if (key == "MemTotal:")
            memory.totalKB = value;
        else if (key == "MemAvailable:")
            memory.availableKB = value;
    }

    if (memory.totalKB == 0 ||
        memory.availableKB > memory.totalKB) {
        throw runtime_error("Invalid memory statistics");
    }

    memory.usedKB = memory.totalKB - memory.availableKB;
    return memory;
}

// ==================== DISK I/O MONITORING ====================

struct DiskStats {
    unsigned long long sectorsRead = 0;
    unsigned long long sectorsWritten = 0;
};

// Count only whole disks (sda, vda, nvme0n1, mmcblk0). Partitions (sda1, nvme0n1p1)
// and virtual devices (loop, ram, dm-) would otherwise be double-counted.
bool isWholeDisk(const string& d) {
    if (d.rfind("loop", 0) == 0 || d.rfind("ram", 0) == 0 ||
        d.rfind("dm-", 0) == 0 || d.rfind("zram", 0) == 0 ||
        d.rfind("sr", 0) == 0)
        return false;
    if (d.rfind("nvme", 0) == 0 || d.rfind("mmcblk", 0) == 0)
    {
        size_t p = d.rfind('p');
        bool isPartition = p != string::npos && p + 1 < d.size() &&
                           isdigit(static_cast<unsigned char>(d[p - 1])) &&
                           d.find_first_not_of("0123456789", p + 1) == string::npos;
        return !isPartition;
    }
    return !isdigit(static_cast<unsigned char>(d.back()));
}

map<string, DiskStats> readDiskStats() {
    ifstream file("/proc/diskstats");

    if (!file)
        throw runtime_error("Unable to read /proc/diskstats");

    map<string, DiskStats> disks;
    string line;

    while (getline(file, line)) {
        istringstream row(line);

        int majorNumber, minorNumber;
        string device;
        unsigned long long readsCompleted, readsMerged, sectorsRead;
        unsigned long long readTime, writesCompleted, writesMerged;
        unsigned long long sectorsWritten, writeTime;

        if (!(row >> majorNumber >> minorNumber >> device
                  >> readsCompleted >> readsMerged >> sectorsRead
                  >> readTime >> writesCompleted >> writesMerged
                  >> sectorsWritten >> writeTime)) {
            continue;
        }

        if (!isWholeDisk(device))
            continue;

        disks[device] = {sectorsRead, sectorsWritten};
    }

    return disks;
}

pair<double, double> calculateDiskRates(
    const map<string, DiskStats>& before,
    const map<string, DiskStats>& after,
    double elapsedSeconds) {

    double readKB = 0.0;
    double writeKB = 0.0;

    for (const auto& entry : after) {
        auto old = before.find(entry.first);
        if (old == before.end())
            continue;

        const auto& current = entry.second;
        const auto& previous = old->second;

        if (current.sectorsRead < previous.sectorsRead ||
            current.sectorsWritten < previous.sectorsWritten) {
            continue;
        }

        // Linux disk statistics count sectors in 512-byte units.
        readKB += (current.sectorsRead - previous.sectorsRead) * 0.5;
        writeKB += (current.sectorsWritten - previous.sectorsWritten) * 0.5;
    }

    return {readKB / elapsedSeconds, writeKB / elapsedSeconds};
}

// ==================== NETWORK MONITORING ====================

struct NetworkStats {
    unsigned long long receivedBytes = 0;
    unsigned long long transmittedBytes = 0;
};

map<string, NetworkStats> readNetworkStats() {
    ifstream file("/proc/net/dev");

    if (!file)
        throw runtime_error("Unable to read /proc/net/dev");

    map<string, NetworkStats> interfaces;
    string line;

    getline(file, line);
    getline(file, line);

    while (getline(file, line)) {
        size_t colon = line.find(':');
        if (colon == string::npos)
            continue;

        string interface = line.substr(0, colon);
        size_t start = interface.find_first_not_of(" \t");
        if (start == string::npos)
            continue;

        interface = interface.substr(start);
        size_t end = interface.find_last_not_of(" \t");
        interface = interface.substr(0, end + 1);

        // Loopback traffic is machine-to-machine (e.g. the dashboard polling Flask),
        // not real network usage, and always has equal RX and TX.
        if (interface == "lo")
            continue;

        istringstream values(line.substr(colon + 1));

        unsigned long long received, receivePackets, receiveErrors;
        unsigned long long receiveDrops, receiveFifo, receiveFrame;
        unsigned long long receiveCompressed, receiveMulticast;
        unsigned long long transmitted, transmitPackets, transmitErrors;
        unsigned long long transmitDrops, transmitFifo, transmitCollisions;
        unsigned long long transmitCarrier, transmitCompressed;

        if (!(values >> received >> receivePackets >> receiveErrors
                     >> receiveDrops >> receiveFifo >> receiveFrame
                     >> receiveCompressed >> receiveMulticast
                     >> transmitted >> transmitPackets >> transmitErrors
                     >> transmitDrops >> transmitFifo >> transmitCollisions
                     >> transmitCarrier >> transmitCompressed)) {
            continue;
        }

        interfaces[interface] = {received, transmitted};
    }

    return interfaces;
}

pair<double, double> calculateNetworkRates(
    const map<string, NetworkStats>& before,
    const map<string, NetworkStats>& after,
    double elapsedSeconds) {

    double downloadKB = 0.0;
    double uploadKB = 0.0;

    for (const auto& entry : after) {
        auto old = before.find(entry.first);
        if (old == before.end())
            continue;

        const auto& current = entry.second;
        const auto& previous = old->second;

        if (current.receivedBytes < previous.receivedBytes ||
            current.transmittedBytes < previous.transmittedBytes) {
            continue;
        }

        downloadKB +=
            (current.receivedBytes - previous.receivedBytes) / 1024.0;
        uploadKB +=
            (current.transmittedBytes - previous.transmittedBytes) / 1024.0;
    }

    return {downloadKB / elapsedSeconds, uploadKB / elapsedSeconds};
}

// ==================== SYSTEM INFO ====================

struct SystemInfo {
    double load1 = 0, load5 = 0, load15 = 0;
    double uptimeSeconds = 0;
    int runningProcesses = 0;
    int totalProcesses = 0;
};

SystemInfo readSystemInfo() {
    SystemInfo info;

    // /proc/loadavg: "0.52 0.58 0.59 2/345 12345"
    ifstream load("/proc/loadavg");
    if (!load)
        throw runtime_error("Unable to read /proc/loadavg");
    string procs;
    load >> info.load1 >> info.load5 >> info.load15 >> procs;
    size_t slash = procs.find('/');
    if (slash != string::npos) {
        info.runningProcesses = stoi(procs.substr(0, slash));
        info.totalProcesses = stoi(procs.substr(slash + 1));
    }

    ifstream up("/proc/uptime");
    if (!up || !(up >> info.uptimeSeconds))
        throw runtime_error("Unable to read /proc/uptime");

    return info;
}

// ==================== DATABASE ====================

sqlite3* openDatabase() {
    sqlite3* db = nullptr;

    mkdir("data", 0755);  // ignore error if it already exists

    if (sqlite3_open("data/resources.db", &db) != SQLITE_OK) {
        string message = db ? sqlite3_errmsg(db)
                            : "Unable to allocate SQLite connection";
        if (db)
            sqlite3_close(db);
        throw runtime_error("Database error: " + message);
    }

    const char* createSQL = R"(
        CREATE TABLE IF NOT EXISTS resource_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            cpu_percent REAL NOT NULL,
            memory_total_kb REAL NOT NULL,
            memory_used_kb REAL NOT NULL,
            memory_percent REAL NOT NULL,
            disk_read_kbps REAL NOT NULL,
            disk_write_kbps REAL NOT NULL,
            network_download_kbps REAL NOT NULL,
            network_upload_kbps REAL NOT NULL,
            load_1 REAL NOT NULL DEFAULT 0,
            load_5 REAL NOT NULL DEFAULT 0,
            load_15 REAL NOT NULL DEFAULT 0,
            uptime_seconds REAL NOT NULL DEFAULT 0,
            running_processes INTEGER NOT NULL DEFAULT 0,
            total_processes INTEGER NOT NULL DEFAULT 0
        );
    )";

    // WAL lets Flask read while this program writes; busy_timeout avoids "database is locked".
    sqlite3_busy_timeout(db, 3000);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    char* errorMessage = nullptr;
    int result = sqlite3_exec(db, createSQL, nullptr, nullptr, &errorMessage);

    if (result != SQLITE_OK) {
        string message = errorMessage ? errorMessage : sqlite3_errmsg(db);
        sqlite3_free(errorMessage);
        sqlite3_close(db);
        throw runtime_error("Unable to create table: " + message);
    }

    // Upgrade databases created by the older version of this program.
    const char* newColumns[] = {
        "load_1 REAL NOT NULL DEFAULT 0", "load_5 REAL NOT NULL DEFAULT 0",
        "load_15 REAL NOT NULL DEFAULT 0", "uptime_seconds REAL NOT NULL DEFAULT 0",
        "running_processes INTEGER NOT NULL DEFAULT 0",
        "total_processes INTEGER NOT NULL DEFAULT 0"};
    for (const char* column : newColumns) {
        string sql = string("ALTER TABLE resource_history ADD COLUMN ") + column + ";";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);  // fails harmlessly if present
    }

    return db;
}

void saveMeasurement(
    sqlite3* db,
    sqlite3_stmt* statement,
    double cpuUsage,
    const MemoryStats& memory,
    double memoryUsage,
    double diskRead,
    double diskWrite,
    double networkDownload,
    double networkUpload,
    const SystemInfo& sys) {

    sqlite3_bind_double(statement, 1, cpuUsage);
    sqlite3_bind_double(statement, 2, static_cast<double>(memory.totalKB));
    sqlite3_bind_double(statement, 3, static_cast<double>(memory.usedKB));
    sqlite3_bind_double(statement, 4, memoryUsage);
    sqlite3_bind_double(statement, 5, diskRead);
    sqlite3_bind_double(statement, 6, diskWrite);
    sqlite3_bind_double(statement, 7, networkDownload);
    sqlite3_bind_double(statement, 8, networkUpload);
    sqlite3_bind_double(statement, 9, sys.load1);
    sqlite3_bind_double(statement, 10, sys.load5);
    sqlite3_bind_double(statement, 11, sys.load15);
    sqlite3_bind_double(statement, 12, sys.uptimeSeconds);
    sqlite3_bind_int(statement, 13, sys.runningProcesses);
    sqlite3_bind_int(statement, 14, sys.totalProcesses);

    if (sqlite3_step(statement) != SQLITE_DONE) {
        string message = sqlite3_errmsg(db);
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        throw runtime_error("Unable to save measurement: " + message);
    }

    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
}

// ==================== MAIN PROGRAM ====================

int main() {
    sqlite3* db = nullptr;
    sqlite3_stmt* insertStatement = nullptr;

    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    try {
        db = openDatabase();

        const char* insertSQL = R"(
            INSERT INTO resource_history (
                cpu_percent,
                memory_total_kb,
                memory_used_kb,
                memory_percent,
                disk_read_kbps,
                disk_write_kbps,
                network_download_kbps,
                network_upload_kbps,
                load_1, load_5, load_15, uptime_seconds,
                running_processes, total_processes
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )";

        if (sqlite3_prepare_v2(
                db, insertSQL, -1, &insertStatement, nullptr) != SQLITE_OK) {
            throw runtime_error(
                string("Unable to prepare database insert: ") +
                sqlite3_errmsg(db));
        }

        CPUStats previousCPU = readCPUStats();
        auto previousDisk = readDiskStats();
        auto previousNetwork = readNetworkStats();

        cout << "====================================\n";
        cout << "       OS RESOURCE MONITOR\n";
        cout << "====================================\n";
        cout << "SQLite logging enabled: data/resources.db\n";
        cout << "Sampling interval: 1 second\n";
        cout << "Press Ctrl+C to stop monitoring.\n";

        long long sampleCount = 0;

        while (keepRunning) {
            auto startTime = chrono::steady_clock::now();
            this_thread::sleep_for(chrono::seconds(1));
            if (!keepRunning)
                break;

            CPUStats currentCPU = readCPUStats();
            auto currentDisk = readDiskStats();
            auto currentNetwork = readNetworkStats();

            auto endTime = chrono::steady_clock::now();
            double elapsedSeconds =
                chrono::duration<double>(endTime - startTime).count();

            if (elapsedSeconds <= 0.0)
                throw runtime_error("Invalid sampling interval");

            double cpuUsage =
                calculateCPUUsage(previousCPU, currentCPU);

            MemoryStats memory = readMemoryStats();
            double memoryUsage =
                100.0 * memory.usedKB / memory.totalKB;

            auto diskRates = calculateDiskRates(
                previousDisk, currentDisk, elapsedSeconds);

            auto networkRates = calculateNetworkRates(
                previousNetwork, currentNetwork, elapsedSeconds);

            saveMeasurement(
                db, insertStatement,
                cpuUsage, memory, memoryUsage,
                diskRates.first, diskRates.second,
                networkRates.first, networkRates.second,
                readSystemInfo());

            cout << fixed << setprecision(2);
            cout << "\n------------------------------------\n";
            cout << "CPU usage: " << cpuUsage << "%\n";
            cout << "Memory: " << memoryUsage << "% ("
                 << memory.usedKB / 1024.0 / 1024.0 << " GB used / "
                 << memory.totalKB / 1024.0 / 1024.0 << " GB total)\n";
            cout << "Disk read: " << diskRates.first << " KB/s\n";
            cout << "Disk write: " << diskRates.second << " KB/s\n";
            cout << "Network download: " << networkRates.first << " KB/s\n";
            cout << "Network upload: " << networkRates.second << " KB/s\n";
            SystemInfo sys = readSystemInfo();
            cout << "Load average: " << sys.load1 << " " << sys.load5 << " "
                 << sys.load15 << "  Processes: " << sys.runningProcesses
                 << " running / " << sys.totalProcesses << " total\n";
            cout << "Saved to database.\n";

            // Prune old rows once a minute.
            if (++sampleCount % 60 == 0) {
                string prune =
                    "DELETE FROM resource_history WHERE id <= "
                    "(SELECT MAX(id) FROM resource_history) - " +
                    to_string(MAX_ROWS) + ";";
                sqlite3_exec(db, prune.c_str(), nullptr, nullptr, nullptr);
            }

            previousCPU = currentCPU;
            previousDisk = currentDisk;
            previousNetwork = currentNetwork;
        }
        cout << "\nStopping monitor...\n";
        sqlite3_finalize(insertStatement);
        sqlite3_close(db);
        return 0;
    } catch (const exception& e) {
        cerr << "\nError: " << e.what() << '\n';

        if (insertStatement)
            sqlite3_finalize(insertStatement);
        if (db)
            sqlite3_close(db);

        return 1;
    }

    return 0;
}
