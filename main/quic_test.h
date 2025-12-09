#pragma once

#include <cstdint>
#include <cstddef>

/**
 * QUIC/HTTP3 Test Functions
 */

/**
 * Test Http3Manager shared connection
 * 
 * Test flow:
 * 1. Establish connection
 * 2. Test concurrent requests (GET / and GET /pocket-sage/health)
 * 3. Wait 30 seconds
 * 4. Test ChatStream (POST /pocket-sage/chat/stream)
 * 5. Don't close connection, wait for server to close
 */
void TestHttp3ManagerSharedConnection(const char* hostname, uint16_t port);

/**
 * Get test audio data (for ChatStream test)
 */
const uint8_t* GetTestOggData();

/**
 * Get test audio data size
 */
size_t GetTestOggSize();

/**
 * Test HTTP3 download speed
 * 
 * Downloads a file from the specified URL and measures the speed.
 * 
 * @param hostname Server hostname
 * @param port Server port
 * @param path URL path to download (e.g., "/testfile.bin")
 */
void TestHttp3DownloadSpeed(const char* hostname, uint16_t port, const char* path);

