#include "Buffer.h"
#include "lang_var.h"

// How long a producer waits for the buffer lock before giving up on its record.
// append() runs in the WiFi and NimBLE tasks, and a card that has gone away can
// keep a flush busy for a long time; blocking a radio task forever is worse than
// losing the record, which add() already does when it cannot keep up.
static const uint32_t BUFFER_LOCK_TIMEOUT_MS = 1000;

// Quiet period after a failed flush before the next attempt. A pulled or dead
// card can make fs->open() slow, and retrying it on every loop() tick stalls the
// scan far more than the wait costs -- the data is kept either way.
static const uint32_t BUFFER_FLUSH_RETRY_MS = 2000;

Buffer::Buffer(){
  this->lock = xSemaphoreCreateRecursiveMutex();

  bufA = (uint8_t*)malloc(BUF_SIZE);
  bufB = (uint8_t*)malloc(BUF_SIZE);

  // Every write path memcpy()s straight into these, so there is nothing to
  // salvage if the heap could not spare them. Refuse to accept data rather than
  // dereference NULL, and raise the same flag a dead card raises so the screen
  // does not report a healthy log.
  if ((this->bufA == NULL) || (this->bufB == NULL)) {
    free(this->bufA);
    free(this->bufB);
    this->bufA = NULL;
    this->bufB = NULL;
    this->buffers_ok = false;
    this->write_failed = true;
    Serial.println(F("Buffer: capture buffers could not be allocated"));
  }
  else {
    this->buffers_ok = true;
  }
}

bool Buffer::lockTake() {
  if (this->lock == NULL) return true;   // no mutex: unsynchronised, as it was before
  return (xSemaphoreTakeRecursive(this->lock, pdMS_TO_TICKS(BUFFER_LOCK_TIMEOUT_MS)) == pdTRUE);
}

void Buffer::lockGive() {
  if (this->lock == NULL) return;
  xSemaphoreGiveRecursive(this->lock);
}

void Buffer::createFile(const char* name, bool is_pcap, bool is_gpx){
  int i=0;
  if (is_pcap) {
    do{
      fileName = "/"+String(name)+"_"+(String)i+".pcap";
      i++;
    } while(fs->exists(fileName));
  }
  else if ((!is_pcap) && (!is_gpx)) {
    do{
      fileName = "/"+String(name)+"_"+(String)i+".log";
      i++;
    } while(fs->exists(fileName));
  }
  else {
    do{
      fileName = "/"+String(name)+"_"+(String)i+".gpx";
      i++;
    } while(fs->exists(fileName));
  }

  Serial.println(fileName);

  file = fs->open(fileName, FILE_WRITE);
  if (!file) {
    // The log does not exist at all. Flag it here rather than waiting for the
    // first flush, otherwise the only symptom is an upload picker with nothing
    // in it long after the drive is over.
    this->write_failed = true;
    Serial.println(text02+fileName+"'");
    return;
  }
  file.close();
}

void Buffer::open(bool is_pcap){
  // Anything still sitting here belongs to the previous file and cannot be
  // filed under the new name, so it goes -- including a tail the card refused.
  // That also bounds retention: a target change always empties the buffer.
  bufSizeA = 0;
  bufSizeB = 0;

  bufSizeB = 0;

  writing = true;

  if (is_pcap) {
    write(uint32_t(0xa1b2c3d4)); // magic number
    write(uint16_t(2)); // major version number
    write(uint16_t(4)); // minor version number
    write(int32_t(0)); // GMT to local correction
    write(uint32_t(0)); // accuracy of timestamps
    write(uint32_t(SNAP_LEN)); // max length of captured packets, in octets
    write(uint32_t(105)); // data link type
  }
}

String Buffer::getFileName() {
  return this->fileName;
}

// Not covered by the lock, and deliberately so: retargeting does SD I/O, and
// holding the mutex across it would park a producer for the whole file-index
// scan. What that leaves exposed is a producer appending between `fs` being
// reassigned here and open() clearing the sizes -- those bytes would be filed
// under the new name. Every caller in this firmware (startPcap/startLog/startGPX
// out of the Run*Scan entry points) runs before its callback is armed, so no
// producer is live at this point; that is an assumption about the callers, not
// something enforced here.
void Buffer::openFile(const char* file_name, fs::FS* fs, bool serial, bool is_pcap, bool is_gpx) {
  // New target, clean health record -- createFile() below raises the flag again
  // if the card will not even make the file.
  this->write_failed = false;
  this->failed_flushes = 0;
  this->dropped_records = 0;
  this->last_flush_ms = 0;

  bool save_pcap = settings_obj.loadSetting<bool>("SavePCAP");
  if (!save_pcap) {
    this->fs = NULL;
    this->serial = false;
    writing = false;
    return;
  }
  this->fs = fs;
  this->serial = serial;
  if (this->fs) {
    createFile(file_name, is_pcap, is_gpx);
  }
  if (this->fs || this->serial) {
    open(is_pcap);
  } else {
    writing = false;
  }
}

void Buffer::pcapOpen(const char* file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, true);
}

void Buffer::logOpen(const char* file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, false);
}

void Buffer::gpxOpen(const char* file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, false, true);
}

void Buffer::add(const uint8_t* buf, uint32_t len, bool is_pcap){
  if(!writing) return;
  if(!this->buffers_ok) return;

  // Hold the lock for the whole record: the pcap header fields and the payload
  // below are separate write() calls, and a flush landing between them would be
  // harmless only by accident.
  if(!this->lockTake()){
    this->dropped_records++;
    return;
  }

  // 16 bytes of pcap record header go in ahead of the payload, so the fullness
  // test has to reserve them as well. It used to look at `len` alone while only
  // the buffer-switch test below allowed for the header, which let a record that
  // "just fits" memcpy up to 16 bytes past the end of the buffer.
  const uint32_t need = len + 16;

  // buffer is full -> drop packet
  if((useA && bufSizeA + need >= BUF_SIZE && bufSizeB > 0) || (!useA && bufSizeB + need >= BUF_SIZE && bufSizeA > 0)){
    //Serial.print(";");
    this->dropped_records++;
    this->lockGive();
    return;
  }

  if(useA && bufSizeA + need >= BUF_SIZE && bufSizeB == 0){
    useA = false;
    //Serial.println("\nswitched to buffer B");
  }
  else if(!useA && bufSizeB + need >= BUF_SIZE && bufSizeA == 0){
    useA = true;
    //Serial.println("\nswitched to buffer A");
  }

  uint32_t microSeconds = micros(); // e.g. 45200400 => 45s 200ms 400us
  uint32_t seconds = (microSeconds/1000)/1000; // e.g. 45200400/1000/1000 = 45200 / 1000 = 45s

  microSeconds -= seconds*1000*1000; // e.g. 45200400 - 45*1000*1000 = 45200400 - 45000000 = 400us (because we only need the offset)

  if (is_pcap) {
    write(seconds); // ts_sec
    write(microSeconds); // ts_usec
    write(len); // incl_len
    write(len); // orig_len
  }

  write(buf, len); // packet payload

  this->lockGive();
}

void Buffer::append(wifi_promiscuous_pkt_t *packet, int len) {
  bool save_packet = settings_obj.loadSetting<bool>(text_table4[7]);
  if (save_packet) {
    add(packet->payload, len, true);
  }
}

void Buffer::append(String log) {
  bool save_packet = settings_obj.loadSetting<bool>(text_table4[7]);
  if (save_packet) {
    add((const uint8_t*)log.c_str(), log.length(), false);
  }
}

void Buffer::write(int32_t n){
  uint8_t buf[4];
  buf[0] = n;
  buf[1] = n >> 8;
  buf[2] = n >> 16;
  buf[3] = n >> 24;
  write(buf,4);
}

void Buffer::write(uint32_t n){
  uint8_t buf[4];
  buf[0] = n;
  buf[1] = n >> 8;
  buf[2] = n >> 16;
  buf[3] = n >> 24;
  write(buf,4);
}

void Buffer::write(uint16_t n){
  uint8_t buf[2];
  buf[0] = n;
  buf[1] = n >> 8;
  write(buf,2);
}

void Buffer::write(const uint8_t* buf, uint32_t len){
  if(!writing) return;
  if(!this->buffers_ok) return;

  // Taken again here because open() writes the pcap file header without going
  // through add(); the mutex is recursive, so the nested take inside a record
  // costs nothing.
  if(!this->lockTake()) return;

  uint8_t* dst = useA ? bufA : bufB;
  uint32_t* size = useA ? &bufSizeA : &bufSizeB;

  // Backstop. add() reserves the room, so this should never fire -- but nothing
  // may memcpy past the end of a heap buffer because a caller got its own
  // arithmetic wrong, and losing the tail of a record beats corrupting the heap.
  // Written as a subtraction so a nonsense length (append() takes `int len` and
  // hands it over as uint32_t) cannot wrap the sum back into range.
  if((*size > BUF_SIZE) || (len > BUF_SIZE - *size)){
    this->dropped_records++;
    this->lockGive();
    return;
  }

  memcpy(&dst[*size], buf, len);
  *size += len;

  this->lockGive();
}

// Write one buffer out, keeping whatever the card refused.
//
// A short write is a failure like any other, but the bytes that did land are
// already on the card: only the remainder may be retried, or the next flush
// would duplicate rows. Returns true only when the whole buffer went out.
bool Buffer::writeChunk(uint8_t* buf, uint32_t &size){
  if(size == 0) return true;

  size_t written = file.write(buf, size);
  if(written == size){
    size = 0;
    return true;
  }

  if(written > 0){
    memmove(buf, buf + written, size - written);
    size -= written;
  }
  return false;
}

bool Buffer::saveFs(){
  file = fs->open(fileName, FILE_APPEND);
  if (!file) {
    Serial.println(text02+fileName+"'");
    return false;
  }

  const uint32_t size_before    = file.size();
  const uint32_t pending_before = bufSizeA + bufSizeB;

  // Order matters and must not change: the buffer being written to goes last,
  // so the file stays in sequence. Short-circuiting on the first failure keeps
  // it that way -- writing the second buffer after the first one only partly
  // landed would file those rows out of order.
  bool ok;
  if(useA){
    ok = writeChunk(bufB, bufSizeB) && writeChunk(bufA, bufSizeA);
  } else {
    ok = writeChunk(bufA, bufSizeA) && writeChunk(bufB, bufSizeB);
  }

  // What the writeChunk calls believe they placed. Anything still sitting in
  // the buffers was refused and is kept for the next flush.
  const uint32_t claimed = pending_before - (bufSizeA + bufSizeB);

  file.close();

  // file.write() can report bytes as accepted while they are still in the stdio
  // buffer, and file.close() returns void, so a card that fills up mid-flush is
  // free to look like a clean write right until the data is pushed to the
  // medium. Once the handle is closed FAT has committed the directory entry, so
  // the size on a fresh open is the one piece of evidence that survives -- the
  // same check the config writer in FileServerAP does, for the same reason.
  //
  // The rows behind a mismatch are already gone; the point is that save() now
  // learns the flush failed, backs off, and the SD health check on the core
  // display stops claiming everything is fine.
  if(ok && claimed > 0){
    File verify = fs->open(fileName, FILE_READ);
    const uint32_t size_after = verify ? (uint32_t)verify.size() : 0;
    if(verify) verify.close();

    if(size_after < size_before + claimed){
      Serial.printf("Buffer: flush lost %u of %u B on '%s' (card full?)\n",
                    (unsigned)(size_before + claimed - size_after),
                    (unsigned)claimed, fileName.c_str());
      ok = false;
    }
  }

  return ok;
}

void Buffer::saveSerial() {
  // Saves to main console UART, user-facing app will ignore these markers
  // Uses / and ] in markers as they are illegal characters for SSIDs
  const char* mark_begin = "[BUF/BEGIN]";
  const size_t mark_begin_len = strlen(mark_begin);
  const char* mark_close = "[BUF/CLOSE]";
  const size_t mark_close_len = strlen(mark_close);

  // Additional buffer and memcpy's so that a single Serial.write() is called
  // This is necessary so that other console output isn't mixed into buffer stream
  uint8_t* buf = (uint8_t*)malloc(mark_begin_len + bufSizeA + bufSizeB + mark_close_len);
  if(buf == NULL){
    // Nothing to memcpy into. Skipping the mirror costs one console dump; the
    // copy exists only so the markers and the payload cannot be interleaved
    // with other output, so there is no piecemeal fallback worth having.
    Serial.println(F("Buffer: no heap for the serial mirror, skipped"));
    return;
  }
  uint8_t* it = buf;
  memcpy(it, mark_begin, mark_begin_len);
  it += mark_begin_len;

  if(useA){
    if(bufSizeB > 0){
      memcpy(it, bufB, bufSizeB);
      it += bufSizeB;
    }
    if(bufSizeA > 0){
      memcpy(it, bufA, bufSizeA);
      it += bufSizeA;
    }
  } else {
    if(bufSizeA > 0){
      memcpy(it, bufA, bufSizeA);
      it += bufSizeA;
    }
    if(bufSizeB > 0){
      memcpy(it, bufB, bufSizeB);
      it += bufSizeB;
    }
  }

  memcpy(it, mark_close, mark_close_len);
  it += mark_close_len;
  Serial.write(buf, it - buf);
  free(buf);
}

void Buffer::save() {
  if(!this->lockTake()) return;

  if((bufSizeA + bufSizeB) == 0){
    this->lockGive();
    return;
  }

  // Sitting out this tick after a failed flush. Return before the console mirror
  // too, or the retained bytes would be re-emitted on every loop() tick for as
  // long as the card stays broken.
  uint32_t now = millis();
  bool attempt_flush = (this->fs == NULL) ||
                       (!this->write_failed) ||
                       ((now - this->last_flush_ms) >= BUFFER_FLUSH_RETRY_MS);
  if(!attempt_flush){
    this->lockGive();
    return;
  }

  // Console mirror first: saveFs() now consumes only the bytes the card actually
  // accepted and clears the sizes as it goes, so the mirror has to read them
  // while they are still there. A retried flush therefore re-mirrors whatever
  // tail the card refused last time -- duplicate console lines in an already
  // broken state, which is a fair price for the file staying in sequence.
  if(this->serial) saveSerial();

  if(this->fs){
    this->last_flush_ms = now;
    if(saveFs()){
      this->write_failed = false;
      this->failed_flushes = 0;
    } else {
      // Keep what did not reach the card so the next attempt retries it. This
      // cannot grow without bound -- the two buffers are a fixed BUF_SIZE each
      // and once both are full add() drops new records, which is the policy the
      // buffer already used when it could not keep up. Dropping the newest
      // rather than the oldest is forced by the layout: this is a raw byte
      // stream with no record index, so there is no front record to discard.
      this->write_failed = true;
      this->failed_flushes++;
    }
  } else {
    // Console-only target: nobody else owns these bytes.
    bufSizeA = 0;
    bufSizeB = 0;
  }

  this->lockGive();
}
