// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QSet>
#include <QJsonObject>
#include <QString>

#include "torrentgovernorcore.h"

namespace BitTorrent
{
struct GovernorConfiguration
{
    bool enabled = false;
    bool valid = true;
    QSet<QString> tags;
    QSet<QString> categories;
    Governor::Config policy;
};

// Active policy is pinned at startup. WebUI may save settings for the next start;
// manual configuration-file edits still require stopping qBittorrent first.
const GovernorConfiguration &governorConfiguration();
QJsonObject governorPreferences();
// Save only; the active policy stays pinned until process restart.
// Return an empty string on success, otherwise a validation/persistence error.
QString saveGovernorPreferences(const QJsonObject &values, const QString &revision);
}
