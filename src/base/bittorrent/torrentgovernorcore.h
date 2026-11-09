// SPDX-License-Identifier: GPL-2.0-or-later
// Pure C++17 policy. No Qt, libtorrent, networking or filesystem access.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace BitTorrent::Governor
{
using Bytes = std::int64_t;

struct Config
{
    Bytes period = 4500;
    Bytes recovery = 10;
    Bytes average = 49LL * 1024 * 1024;
    Bytes releaseAverage = 48LL * 1024 * 1024;
    Bytes buffer = 5LL * 1024 * 1024 * 1024;
    int softUpload = 30 * 1024 * 1024;
    int brakeUpload = 10 * 1024;
    int brakeDownload = 10 * 1024;
    double uploadProgress = 0.80;
    double downloadProgress = 0.97;
    Bytes lookahead = 5;

    bool valid() const
    {
        return (period >= 10) && (period <= 86400 * 30)
            && (recovery >= 0) && (recovery < period)
            && (average > 0) && (average <= std::numeric_limits<int>::max())
            && (releaseAverage > 0) && (releaseAverage < average)
            && (buffer >= 0) && (buffer < average * period)
            && (softUpload > 0) && (softUpload < average)
            && (brakeUpload > 0) && (brakeUpload <= softUpload)
            && (brakeDownload > 0)
            && std::isfinite(uploadProgress) && std::isfinite(downloadProgress)
            && (uploadProgress >= 0) && (uploadProgress <= downloadProgress)
            && (downloadProgress <= 1) && (lookahead >= 0) && (lookahead <= 300);
    }

    Bytes threshold() const { return average * period - buffer; }

    std::string signature() const
    {
        std::ostringstream out;
        out << std::setprecision(17) << "gov1 " << period << ' ' << recovery << ' '
            << average << ' ' << releaseAverage << ' ' << buffer << ' '
            << softUpload << ' ' << brakeUpload << ' ' << brakeDownload << ' '
            << uploadProgress << ' ' << downloadProgress << ' ' << lookahead;
        return out.str();
    }
};

inline int combineLimits(const int user, const int policy)
{
    if (user <= 0) return std::max(0, policy);
    if (policy <= 0) return user;
    return std::min(user, policy);
}

struct Decision
{
    int upload = 0;    // 0 = no additional policy limit
    int download = 0;
    bool uncertain = false;
    bool hard = false;
    bool soft = false;
    bool downloadLatched = false;
    bool recovering = false;
    Bytes start = 0;
    Bytes used = 0;
    Bytes age = 1;
    double average = 0;
};

class Engine
{
public:
    explicit Engine(const Config &config = {}) : m_config(config) {}

    void invalidate() { m_uncertain = true; }

    Decision sample(Bytes added, Bytes now, Bytes total, double progress, Bytes rate,
        const bool clockFault = false)
    {
        now = std::max<Bytes>(1, now);
        added = (added > 0) ? std::min(added, now) : now;
        total = std::max<Bytes>(0, total);
        progress = std::isfinite(progress) ? std::clamp(progress, 0.0, 1.0) : 0.0;
        rate = std::max<Bytes>(0, rate);

        if (!m_initialized || (added != m_added))
        {
            const bool previouslyUncertain = m_uncertain;
            *this = Engine(m_config);
            m_initialized = true;
            m_added = added;
            m_start = bucket(now);
            m_lastNow = now;
            m_lastTotal = total;
            // A nonzero lifetime counter cannot reconstruct the current bucket.
            m_uncertain = previouslyUncertain || (total != 0) || clockFault;
        }

        const bool backwards = (now < m_lastNow);
        now = std::max(now, m_lastNow);
        const bool counterReset = (total < m_lastTotal);
        const Bytes delta = counterReset ? 0 : total - m_lastTotal;
        const Bytes nextStart = bucket(now);

        if (nextStart > m_start)
        {
            // Conservatively attribute the entire boundary-spanning sample to BOTH
            // buckets. This can overcount, but does not silently discard traffic.
            m_used = add(m_used, delta);
            evaluate(progress, rate, m_config.period);
            m_holdUpload = m_hard || m_uncertain ? m_config.brakeUpload
                : (m_soft ? m_config.softUpload : 0);
            m_holdDownload = m_download;
            m_holdUntil = nextStart + m_config.recovery;
            m_start = nextStart;
            m_used = delta;
            m_hard = false;
            m_soft = false;
            m_download = false;
            m_uncertain = false;
        }
        else
        {
            m_used = add(m_used, delta);
        }

        if (backwards || counterReset || clockFault) m_uncertain = true;
        m_lastNow = now;
        m_lastTotal = total;
        const Bytes age = std::max<Bytes>(1, now - m_start);
        evaluate(progress, rate, age);

        Decision result;
        result.upload = (m_hard || m_uncertain) ? m_config.brakeUpload
            : (m_soft ? m_config.softUpload : 0);
        result.download = m_download ? m_config.brakeDownload : 0;
        result.recovering = (now < m_holdUntil);
        if (result.recovering)
        {
            result.upload = combineLimits(result.upload, m_holdUpload);
            if (m_holdDownload)
                result.download = combineLimits(result.download, m_config.brakeDownload);
        }
        result.uncertain = m_uncertain;
        result.hard = m_hard;
        result.soft = m_soft;
        result.downloadLatched = m_download;
        result.start = m_start;
        result.used = m_used;
        result.age = age;
        result.average = static_cast<double>(m_used) / age;
        return result;
    }

    std::string save() const
    {
        if (!m_initialized) return {};
        std::ostringstream out;
        out << m_config.signature() << '\n' << m_added << ' ' << m_start << ' '
            << m_lastNow << ' ' << m_lastTotal << ' ' << m_used << ' '
            << m_uncertain << ' ' << m_hard << ' ' << m_soft << ' ' << m_download
            << ' ' << m_holdUntil << ' ' << m_holdUpload << ' ' << m_holdDownload;
        return out.str();
    }

    bool restore(const std::string &text)
    {
        std::istringstream input(text);
        std::string signature;
        std::getline(input, signature);
        if (signature != m_config.signature()) return false;
        Engine candidate(m_config);
        input >> candidate.m_added >> candidate.m_start >> candidate.m_lastNow
            >> candidate.m_lastTotal >> candidate.m_used >> candidate.m_uncertain
            >> candidate.m_hard >> candidate.m_soft >> candidate.m_download
            >> candidate.m_holdUntil >> candidate.m_holdUpload >> candidate.m_holdDownload;
        if (!input || (candidate.m_added <= 0) || (candidate.m_start < candidate.m_added)
            || (candidate.m_lastNow < candidate.m_start) || (candidate.m_lastTotal < 0)
            || (candidate.m_used < 0) || (candidate.m_holdUntil < 0)
            || (candidate.m_holdUntil > candidate.m_start + m_config.recovery)
            || ((candidate.m_holdUpload != 0)
                && (candidate.m_holdUpload != m_config.brakeUpload)
                && (candidate.m_holdUpload != m_config.softUpload))
            || (candidate.bucket(candidate.m_lastNow) != candidate.m_start)) return false;
        input >> std::ws;
        if (!input.eof()) return false;
        candidate.m_initialized = true;
        *this = candidate;
        return true;
    }

private:
    static Bytes add(const Bytes left, const Bytes right)
    {
        return (right > std::numeric_limits<Bytes>::max() - left)
            ? std::numeric_limits<Bytes>::max() : left + right;
    }

    Bytes bucket(const Bytes now) const
    {
        return m_added + ((now - m_added) / m_config.period) * m_config.period;
    }

    void evaluate(const double progress, const Bytes rate, const Bytes age)
    {
        const Bytes remaining = m_config.threshold() - std::min(m_used, m_config.threshold());
        const bool approaching = (rate > 0) && (m_config.lookahead > 0)
            && (static_cast<long double>(rate) * m_config.lookahead >= remaining);
        if ((m_used >= m_config.threshold()) || approaching) m_hard = true;
        const long double average = static_cast<long double>(m_used) / std::max<Bytes>(1, age);
        if (progress < m_config.uploadProgress) m_soft = false;
        else if (average > m_config.average) m_soft = true;
        else if (average <= m_config.releaseAverage) m_soft = false;
        if ((progress >= m_config.downloadProgress)
            && ((average > m_config.average) || m_uncertain)) m_download = true;
    }

    Config m_config;
    bool m_initialized = false;
    Bytes m_added = 0;
    Bytes m_start = 0;
    Bytes m_lastNow = 0;
    Bytes m_lastTotal = 0;
    Bytes m_used = 0;
    bool m_uncertain = false;
    bool m_hard = false;
    bool m_soft = false;
    bool m_download = false;
    Bytes m_holdUntil = 0;
    int m_holdUpload = 0;
    bool m_holdDownload = false;
};
}
