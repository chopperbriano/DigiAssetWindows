//
// Created by DigiAsset Core on 22/07/26.
//

#include "AssetIconProvider.h"
#include "Config.h"
#include <QComboBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>

AssetIconProvider::AssetIconProvider(QObject *parent) : QObject(parent), _iconSize(16), _dgbCore() {
    _dgbCore.setFileName("config.cfg", true);
    _dgbCore.makeConnection();
    _net = new QNetworkAccessManager(this);

    //the icon bytes come straight from the IPFS node's API(same one the daemon uses)
    try {
        Config config("config.cfg");
        _ipfsApi = QString::fromStdString(config.getString("ipfspath", "http://localhost:5001/api/v0/"));
        _iconSize = config.getInteger("guiasseticonsize", 16);
    } catch (...) {
        _ipfsApi = "http://localhost:5001/api/v0/";
    }
    if (_iconSize < 8) _iconSize = 8; //keep it visible but sane
    if (_iconSize > 64) _iconSize = 64;
}

QIcon AssetIconProvider::icon(uint64_t assetIndex) {
    loadAssetInfo(assetIndex);
    auto it = _icons.find(assetIndex);
    return (it != _icons.end()) ? it->second : QIcon();
}

QString AssetIconProvider::name(uint64_t assetIndex) {
    loadAssetInfo(assetIndex);
    auto it = _names.find(assetIndex);
    return (it != _names.end()) ? it->second : QString();
}

/**
 * Fetches the asset's metadata once and caches its display name plus starts the icon
 * download if the metadata lists one(data.urls entry named "icon" on ipfs)
 */
void AssetIconProvider::loadAssetInfo(uint64_t assetIndex) {
    if (_infoLoaded.count(assetIndex)) return;
    _infoLoaded.insert(assetIndex);
    try {
        Json::Value args = Json::arrayValue;
        args.append(static_cast<Json::UInt64>(assetIndex));
        Json::Value data = _dgbCore.sendcommand("getassetdata", args);
        const Json::Value &meta = data["ipfs"]["data"];
        if (meta.isMember("assetName") && meta["assetName"].isString()) {
            _names[assetIndex] = QString::fromStdString(meta["assetName"].asString());
        }
        if (meta.isMember("urls") && meta["urls"].isArray()) {
            for (const auto &url: meta["urls"]) {
                if (!url.isObject() || !url.isMember("name") || !url.isMember("url")) continue;
                if (url["name"].asString() != "icon") continue;
                //some test assets point "icon" at non-image data(e.g. application/json) - those
                //can't be shown as an icon, so skip anything not declared as an image
                if (url.isMember("mimeType") && url["mimeType"].isString() &&
                    url["mimeType"].asString().rfind("image/", 0) != 0) {
                    break;
                }
                std::string target = url["url"].asString();
                if (target.rfind("ipfs://", 0) == 0) {
                    fetchIcon(assetIndex, QString::fromStdString(target.substr(7)));
                }
                break;
            }
        }
    } catch (const DigiByteException &) {
        //metadata may not have synced yet - try again on a later request
        _infoLoaded.erase(assetIndex);
    }
}

///downloads the icon bytes from the IPFS node(api "cat" call) and caches them as a QIcon
void AssetIconProvider::fetchIcon(uint64_t assetIndex, const QString &cid) {
    if (_iconInFlight.count(assetIndex) || _icons.count(assetIndex)) return;
    _iconInFlight.insert(assetIndex);
    QNetworkRequest request(QUrl(_ipfsApi + "cat?arg=" + cid));
    QNetworkReply *reply = _net->post(request, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, assetIndex, reply]() {
        _iconInFlight.erase(assetIndex);
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        QPixmap pixmap;
        if (!pixmap.loadFromData(reply->readAll())) return;
        _icons[assetIndex] = QIcon(pixmap);
        emit iconReady((quint64) assetIndex);
    });
}

void AssetIconProvider::applyToCombo(QComboBox *combo, uint64_t assetIndex, const QIcon &icon) {
    if ((combo == nullptr) || icon.isNull()) return;
    for (int i = 0; i < combo->count(); i++) {
        if (combo->itemData(i).toULongLong() == assetIndex) combo->setItemIcon(i, icon);
    }
}
