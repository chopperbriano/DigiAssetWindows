//
// Created by mctrivia on 27/12/24.
//

#ifndef LOADER_H
#define LOADER_H

#include "DigiByteCore.h"

class RPCLoader {
public:
    RPCLoader();
    DigiByteCore& getCore();

private:
    DigiByteCore dgb;
    bool isConnected;
};

#endif // LOADER_H
