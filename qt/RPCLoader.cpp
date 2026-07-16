#include "RPCLoader.h"
#include <QLabel>
#include <QProgressBar>
#include <QTimer>

RPCLoader::RPCLoader() : isConnected(false) {
    dgb.setFileName("config.cfg", true);
    dgb.makeConnection();
    isConnected = true;
}

DigiByteCore& RPCLoader::getCore() {
    return dgb;
}
