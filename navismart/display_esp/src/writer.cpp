#include "writer.h"

#include <ctime>
#include <fstream>
#include <iomanip>

void writer_write(const MainData &m, const CompData &c) {
    std::ofstream file("display_esp_records.csv", std::ios::app);
    if (!file) {
        return;
    }

    time_t now = time(nullptr);
    struct tm *tm_info = localtime(&now);
    char timestamp[20] = "1970-01-01 00:00:00";
    if (tm_info) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    }

    file << timestamp << ','
         << m.time << ','
         << std::fixed << std::setprecision(6)
         << c.latitude << ','
         << c.longitude << ','
         << std::setprecision(2)
         << m.sog << ','
         << c.cog << ','
         << m.heel << ','
         << m.pitch << ','
         << m.trim << '\n';
}
