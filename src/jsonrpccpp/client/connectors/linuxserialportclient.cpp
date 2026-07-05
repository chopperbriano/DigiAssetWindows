/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    serialportclient.cpp
 * @date    01.01.2019
 * @author  Billy Araujo
 * @license See attached LICENSE.txt
 ************************************************************************/

// ---------------------------------------------------------------------------
// Role in DigiAsset for Windows:
//   Implements LinuxSerialPortClient, a POSIX serial-port transport for the
//   bundled libjson-rpc-cpp JSON-RPC client stack. A JSON-RPC request is
//   written to a serial device and the framed response is read back, letting
//   an rpc client speak to a peer over a serial line instead of a socket.
//   This is a Linux/UNIX-only connector (uses termios/fcntl/unistd) and is not
//   part of the Windows node or pool-server build; it ships only to keep the
//   vendored library source complete.
// ---------------------------------------------------------------------------

#include "linuxserialportclient.h"
#include "../../common/sharedconstants.h"
#include "../../common/streamreader.h"
#include "../../common/streamwriter.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <termios.h>
#include <unistd.h>

using namespace jsonrpc;
using namespace std;

LinuxSerialPortClient::LinuxSerialPortClient(const std::string &deviceName) : deviceName(deviceName) {}

LinuxSerialPortClient::~LinuxSerialPortClient() {}

/**
 * @brief Sends a JSON-RPC request over the serial device and returns the reply.
 *
 * Opens the device, writes message followed by DEFAULT_DELIMITER_CHAR as the
 * frame terminator, then reads the response up to the same delimiter into
 * result. The device is closed before returning.
 * @param message The JSON-RPC request to send.
 * @param result  Receives the raw JSON-RPC response read back from the device.
 * @throw JsonRpcException on write or read failure.
 */
void LinuxSerialPortClient::SendRPCMessage(const std::string &message, std::string &result) {
  int serial_fd = this->Connect();

  StreamWriter writer;
  string toSend = message + DEFAULT_DELIMITER_CHAR;
  if (!writer.Write(toSend, serial_fd)) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Could not write request");
  }

  StreamReader reader(DEFAULT_BUFFER_SIZE);
  if (!reader.Read(result, serial_fd, DEFAULT_DELIMITER_CHAR)) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Could not read response");
  }
  close(serial_fd);
}

/**
 * @brief Opens the serial device named by the constructor for reading/writing.
 * @returns The file descriptor from open(), or a negative value on failure.
 */
int LinuxSerialPortClient::Connect() {

  int serial_fd = open(deviceName.c_str(), O_RDWR);

  return serial_fd;
}

/**
 * @brief Opens the given serial device for reading/writing.
 * @param deviceName The device path to open.
 * @returns The file descriptor from open(), or -1 on failure.
 */
int LinuxSerialPortClient::Connect(const string &deviceName) {

  int serial_fd;

  try {
    serial_fd = open(deviceName.c_str(), O_RDWR);
  } catch (const JsonRpcException &e) {
    serial_fd = -1;
  }

  return serial_fd;
}
