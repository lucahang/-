#pragma once

// 🚀 防止 windows.h 引入 winsock.h
#define _WINSOCKAPI_
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")