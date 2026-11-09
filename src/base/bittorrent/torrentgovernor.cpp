// SPDX-License-Identifier: GPL-2.0-or-later
#include "torrentgovernor.h"

#include <cstdlib>
#include <cmath>

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QThread>

#include "base/logger.h"
#include "base/settingsstorage.h"
#include "session.h"
#include "torrentimpl.h"

using namespace BitTorrent;

namespace
{
    GovernorConfiguration readGovernorConfiguration()
    {
        GovernorConfiguration c;
        const auto *settings = SettingsStorage::instance();
        const QString prefix = QStringLiteral("BitTorrent/Governor/");
        c.enabled = settings->loadValue<bool>(prefix + "Enabled", false);
        const auto readSet = [&](const QString &key, const QString &fallback)
        {
            const QVariant value = settings->loadValue<QVariant>(prefix + key, fallback);
            const QString text = (value.type() == QVariant::StringList)
                ? value.toStringList().join(',') : value.toString();
            QSet<QString> result;
            for (const auto &item : text.split(','))
                if (!item.trimmed().isEmpty()) result.insert(item.trimmed());
            return result;
        };
        c.tags = readSet("TargetTags", "u2");
        c.categories = readSet("TargetCategories", "u2");
        const auto readInt = [&](const QString &key, const qlonglong fallback)
        {
            bool ok = false;
            const qlonglong result = settings->loadValue<QVariant>(prefix + key, fallback).toLongLong(&ok);
            c.valid = c.valid && ok;
            return ok ? result : fallback;
        };
        const auto readRate = [&](const QString &key, const int fallback)
        {
            const auto result = readInt(key, fallback);
            if ((result <= 0) || (result > std::numeric_limits<int>::max()))
            {
                c.valid = false;
                return fallback;
            }
            return static_cast<int>(result);
        };
        const auto readProgress = [&](const QString &key, const double fallback)
        {
            bool ok = false;
            const double result = settings->loadValue<QVariant>(prefix + key, fallback).toDouble(&ok);
            c.valid = c.valid && ok;
            return ok ? result : fallback;
        };
        c.policy.period = readInt("PeriodSeconds", c.policy.period);
        c.policy.recovery = readInt("RecoveryDelaySeconds", c.policy.recovery);
        c.policy.average = readInt("AverageUploadBps", c.policy.average);
        c.policy.releaseAverage = readInt("ReleaseAverageBps", c.policy.releaseAverage);
        c.policy.buffer = readInt("BrakeBufferBytes", c.policy.buffer);
        c.policy.softUpload = readRate("SoftUploadBps", c.policy.softUpload);
        c.policy.brakeUpload = readRate("BrakeUploadBps", c.policy.brakeUpload);
        c.policy.brakeDownload = readRate("BrakeDownloadBps", c.policy.brakeDownload);
        c.policy.uploadProgress = readProgress("UploadProgress", c.policy.uploadProgress);
        c.policy.downloadProgress = readProgress("DownloadProgress", c.policy.downloadProgress);
        c.policy.lookahead = readInt("LookaheadSeconds", c.policy.lookahead);
        c.valid = c.valid && c.policy.valid();
        // Keep representable stored values for the editor. Malformed individual
        // fields use defaults and leave valid=false; only the active policy
        // replaces the WHOLE invalid policy with its safe fallback.
        return c;
    }

    QString joined(const QSet<QString> &values)
    {
        QStringList list = values.values();
        list.sort(Qt::CaseSensitive);
        return list.join(',');
    }

    QJsonObject configurationJson(const GovernorConfiguration &c)
    {
        const auto &p = c.policy;
        return {{"Enabled", c.enabled}, {"TargetTags", joined(c.tags)},
            {"TargetCategories", joined(c.categories)},
            {"PeriodSeconds", static_cast<double>(p.period)},
            {"RecoveryDelaySeconds", static_cast<double>(p.recovery)},
            {"AverageUploadBps", static_cast<double>(p.average)},
            {"ReleaseAverageBps", static_cast<double>(p.releaseAverage)},
            {"BrakeBufferBytes", static_cast<double>(p.buffer)},
            {"SoftUploadBps", p.softUpload}, {"BrakeUploadBps", p.brakeUpload},
            {"BrakeDownloadBps", p.brakeDownload}, {"UploadProgress", p.uploadProgress},
            {"DownloadProgress", p.downloadProgress}, {"LookaheadSeconds", static_cast<double>(p.lookahead)}};
    }

    QString configurationRevision(const QJsonObject &values, const bool valid)
    {
        const QByteArray bytes = QJsonDocument(values).toJson(QJsonDocument::Compact)
            + (valid ? " valid" : " invalid");
        return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    }

    bool validTargets(const QJsonValue &value)
    {
        if (!value.isString() || (value.toString().size() > 4096)) return false;
        const QString text = value.toString();
        const QStringList parts = text.split(',');
        if (parts.size() > 128) return false;
        for (const auto &part : parts)
            if (part.trimmed().size() > 255) return false;
        for (const QChar ch : text)
            if (ch.category() == QChar::Other_Control) return false;
        return true;
    }
}

const GovernorConfiguration &BitTorrent::governorConfiguration()
{
    static const GovernorConfiguration config = []()
    {
        auto c = readGovernorConfiguration();
        if (!c.valid)
        {
            c.policy = {};
            LogMsg(QStringLiteral("Governor: invalid configuration; matching torrents will remain conservatively braked."), Log::CRITICAL);
        }
        if (c.enabled)
            LogMsg(QStringLiteral("Governor enabled: per-torrent accounting; NOT synchronized to tracker announces."), Log::WARNING);
        return c;
    }();
    return config;
}

QJsonObject BitTorrent::governorPreferences()
{
    const auto &active = governorConfiguration();
    const auto stored = readGovernorConfiguration();
    const auto activeJson = configurationJson(active);
    const auto savedJson = configurationJson(stored);
    GovernorConfiguration defaults;
    defaults.tags.insert(QStringLiteral("u2"));
    defaults.categories.insert(QStringLiteral("u2"));
    return {{"active", activeJson}, {"saved", savedJson},
        {"defaults", configurationJson(defaults)}, {"saved_valid", stored.valid},
        {"active_valid", active.valid},
        {"pending_restart", (activeJson != savedJson) || (active.valid != stored.valid)},
        {"revision", configurationRevision(savedJson, stored.valid)}};
}

QString BitTorrent::saveGovernorPreferences(const QJsonObject &values, const QString &revision)
{
    const auto current = governorPreferences();
    if (revision != current.value("revision").toString())
        return QStringLiteral("conflict");
    const auto expected = current.value("defaults").toObject();
    if (values.size() != expected.size())
        return QStringLiteral("请提交完整的 Governor 参数。");
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
        if (!expected.contains(it.key())) return QStringLiteral("包含未知的 Governor 参数。");
    if (!values.value("Enabled").isBool()) return QStringLiteral("启用状态必须是布尔值。");
    if (!validTargets(values.value("TargetTags")) || !validTargets(values.value("TargetCategories")))
        return QStringLiteral("标签或分类过长、数量过多或含控制字符。");

    bool valid = true;
    const auto integer = [&](const char *key, const qint64 low, const qint64 high) -> qint64
    {
        const auto value = values.value(QLatin1String(key));
        const double number = value.toDouble(-1);
        if (!value.isDouble() || !std::isfinite(number) || (std::floor(number) != number)
            || (number < low) || (number > high))
        {
            valid = false;
            return low;
        }
        return static_cast<qint64>(number);
    };
    const auto fraction = [&](const char *key)
    {
        const auto value = values.value(QLatin1String(key));
        const double number = value.toDouble(-1);
        if (!value.isDouble() || !std::isfinite(number) || (number < 0) || (number > 1)) valid = false;
        return number;
    };
    Governor::Config p;
    p.period = integer("PeriodSeconds", 10, 2592000);
    p.recovery = integer("RecoveryDelaySeconds", 0, 2591999);
    p.average = integer("AverageUploadBps", 1, std::numeric_limits<int>::max());
    p.releaseAverage = integer("ReleaseAverageBps", 1, std::numeric_limits<int>::max());
    p.buffer = integer("BrakeBufferBytes", 0,
        static_cast<qint64>(std::numeric_limits<int>::max()) * 2592000);
    p.softUpload = static_cast<int>(integer("SoftUploadBps", 1, std::numeric_limits<int>::max()));
    p.brakeUpload = static_cast<int>(integer("BrakeUploadBps", 1, std::numeric_limits<int>::max()));
    p.brakeDownload = static_cast<int>(integer("BrakeDownloadBps", 1, std::numeric_limits<int>::max()));
    p.uploadProgress = fraction("UploadProgress");
    p.downloadProgress = fraction("DownloadProgress");
    p.lookahead = integer("LookaheadSeconds", 0, 300);
    if (!valid || !p.valid())
        return QStringLiteral("参数不合法：解除均速和软限速须低于触发均速；上传刹车须不高于软限速；缓冲须小于周期预算；恢复延迟须小于周期；下载介入进度须不低于上传介入进度。");

    auto *settings = SettingsStorage::instance();
    const QString prefix = QStringLiteral("BitTorrent/Governor/");
    QHash<QString, QVariant> previous;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
    {
        const QString key = prefix + it.key();
        previous.insert(key, settings->loadValue<QVariant>(key));
        // Store integer fields as 64-bit integers, not locale-sensitive doubles.
        const QVariant stored = it.value().isDouble() && !it.key().endsWith("Progress")
            ? QVariant::fromValue(static_cast<qlonglong>(it.value().toDouble())) : it.value().toVariant();
        settings->storeValue(key, stored);
    }
    if (!settings->save())
    {
        for (auto it = previous.constBegin(); it != previous.constEnd(); ++it)
        {
            if (it.value().isValid()) settings->storeValue(it.key(), it.value());
            else settings->removeValue(it.key());
        }
        settings->save();
        return QStringLiteral("配置写入失败，未确认保存成功。请检查磁盘空间和配置目录权限，然后重新读取。");
    }
    LogMsg(QStringLiteral("Governor configuration saved through WebUI; restart required for activation."), Log::INFO);
    return {};
}

void TorrentImpl::updateGovernor()
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto &config = governorConfiguration();
    if (!config.enabled) return;

    const qint64 wall = QDateTime::currentSecsSinceEpoch();
    bool clockFault = !config.valid || m_governorRestartUncertain;
    m_governorRestartUncertain = false;
    if (m_governorClock.isValid())
    {
        const qint64 elapsed = m_governorClock.restart();
        clockFault = clockFault || (std::llabs((wall - m_governorLastWall) * 1000 - elapsed) > 5000);
    }
    else m_governorClock.start();
    m_governorLastWall = wall;
    m_governorDecision = m_governor.sample(addedTime().toSecsSinceEpoch(), wall,
        totalUpload(), progress(), uploadPayloadRate(), clockFault);

    // Track ALL torrents while enabled; changing tags cannot reset the ledger.
    m_governorMatched = config.categories.contains(category());
    for (const auto &tag : tags()) m_governorMatched = m_governorMatched || config.tags.contains(tag);
    const int upload = m_governorMatched ? m_governorDecision.upload : 0;
    const int download = m_governorMatched ? m_governorDecision.download : 0;
    if ((upload != m_governorUploadLimit) || (download != m_governorDownloadLimit))
    {
        m_governorUploadLimit = upload;
        m_governorDownloadLimit = download;
        applyGovernorLimits();
        LogMsg(QStringLiteral("Governor [%1]: policy U=%2 D=%3 B/s; uncertain=%4; used=%5 bytes")
            .arg(id().toString()).arg(upload).arg(download)
            .arg(m_governorDecision.uncertain).arg(static_cast<qlonglong>(m_governorDecision.used)), Log::INFO);
        m_session->handleTorrentNeedSaveResumeData(this);
    }
    else applyGovernorLimits();

    // Existing resume writer is atomic. A crash can still lose uncheckpointed
    // network traffic, so every restored job starts conservatively uncertain.
    if (!m_governorCheckpoint.isValid() || (m_governorCheckpoint.elapsed() >= 30000))
    {
        m_governorCheckpoint.start();
        m_session->handleTorrentNeedSaveResumeData(this);
    }
}

void TorrentImpl::applyGovernorLimits()
{
    const int up = Governor::combineLimits(m_userUploadLimit, m_governorUploadLimit);
    const int down = Governor::combineLimits(m_userDownloadLimit, m_governorDownloadLimit);
    if (m_appliedUploadLimit != up)
    {
        m_nativeHandle.set_upload_limit(up);
        m_appliedUploadLimit = up;
    }
    if (m_appliedDownloadLimit != down)
    {
        m_nativeHandle.set_download_limit(down);
        m_appliedDownloadLimit = down;
    }
}

QJsonObject TorrentImpl::governorStatus() const
{
    const auto &c = governorConfiguration();
    const auto &d = m_governorDecision;
    return {{"enabled", c.enabled}, {"configuration_valid", c.valid}, {"matched", m_governorMatched},
        {"uncertain", d.uncertain}, {"hard_brake", d.hard}, {"soft_limit", d.soft},
        {"download_latched", d.downloadLatched}, {"recovery_hold", d.recovering},
        {"period_start", static_cast<double>(d.start)},
        {"period_upload_bytes", QString::number(static_cast<qlonglong>(d.used))},
        {"period_average_bps", d.average},
        {"policy_upload_bps", m_governorUploadLimit}, {"policy_download_bps", m_governorDownloadLimit},
        {"effective_upload_bps", m_nativeHandle.upload_limit()},
        {"effective_download_bps", m_nativeHandle.download_limit()}};
}
