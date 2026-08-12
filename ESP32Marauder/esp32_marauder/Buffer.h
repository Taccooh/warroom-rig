#pragma once

#ifndef Buffer_h
#define Buffer_h

#include "Arduino.h"
#include "FS.h"
#include "settings.h"
#include "esp_wifi_types.h"
#include "configs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

//#define BUF_SIZE 3 * 1024 // Had to reduce buffer size to save RAM. GG @spacehuhn
//#define SNAP_LEN 2324 // max len of each recieved packet

//extern bool useSD;

extern Settings settings_obj;

class Buffer {
  public:
    Buffer();
    void pcapOpen(const char* file_name, fs::FS* fs, bool serial);
    void logOpen(const char* file_name, fs::FS* fs, bool serial);
    void gpxOpen(const char* file_name, fs::FS* fs, bool serial);
    void append(wifi_promiscuous_pkt_t *packet, int len);
    void append(String log);
    void save();
    String getFileName();

    // --- Write health, for whoever draws the screen --------------------------
    // A card that is missing, full, write-protected or throwing FATFS errors was
    // indistinguishable from a working one: saveFs() returned early on a failed
    // open, ignored the return of file.write(), and save() cleared the buffer
    // either way. The scan views count their own rows, so the operator watched
    // the numbers climb while the card took nothing. There is no console in the
    // field, so the answer has to be state someone can render, not a Serial
    // print. Nothing in here allocates or blocks; it is safe to poll per frame.
    bool writeFailed() const { return this->write_failed; }          // last flush did not land
    uint32_t failedFlushes() const { return this->failed_flushes; }  // consecutive failures
    uint32_t droppedRecords() const { return this->dropped_records; }// never made it into RAM
    uint32_t pendingBytes() const { return this->bufSizeA + this->bufSizeB; }
  private:
    void createFile(const char* name, bool is_pcap, bool is_gpx = false);
    void open(bool is_pcap);
    void openFile(const char* file_name, fs::FS* fs, bool serial, bool is_pcap, bool is_gpx = false);
    void add(const uint8_t* buf, uint32_t len, bool is_pcap);
    void write(int32_t n);
    void write(uint32_t n);
    void write(uint16_t n);
    void write(const uint8_t* buf, uint32_t len);
    bool writeChunk(uint8_t* buf, uint32_t &size);
    bool saveFs();
    void saveSerial();

    // Every buffer mutation goes through these. append() runs from the WiFi
    // promiscuous callback and from the NimBLE scan callback while save() runs
    // from loop(), so the old "while (saving) delay(10)" could be passed by one
    // task in the instant before the other set the flag. Recursive because add()
    // holds it for a whole record while the write() helpers take it again per
    // field, and because open() calls write() directly.
    bool lockTake();
    void lockGive();
    SemaphoreHandle_t lock = NULL;

    uint8_t* bufA = NULL;
    uint8_t* bufB = NULL;
    bool buffers_ok = false; // both allocations came back

    uint32_t bufSizeA = 0;
    uint32_t bufSizeB = 0;

    volatile bool writing = false; // acceppting writes to buffer
    bool useA = true; // writing to bufA or bufB

    volatile bool write_failed = false;
    uint32_t failed_flushes = 0;
    uint32_t dropped_records = 0;
    uint32_t last_flush_ms = 0;

    String fileName = "/0.pcap";
    File file;
    fs::FS* fs = NULL;
    bool serial = false;
};

#endif
