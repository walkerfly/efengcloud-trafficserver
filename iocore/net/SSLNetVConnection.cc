/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */
#include "ink_config.h"
#include "records/I_RecHttp.h"
#include "P_Net.h"
#include "P_SSLNextProtocolSet.h"
#include "P_SSLUtils.h"
#include "InkAPIInternal.h"	// Added to include the ssl_hook definitions

#define SSL_READ_ERROR_NONE	  0
#define SSL_READ_ERROR		  1
#define SSL_READ_READY		  2
#define SSL_READ_COMPLETE	  3
#define SSL_READ_WOULD_BLOCK      4
#define SSL_READ_EOS		  5
#define SSL_HANDSHAKE_WANT_READ   6
#define SSL_HANDSHAKE_WANT_WRITE  7
#define SSL_HANDSHAKE_WANT_ACCEPT 8
#define SSL_HANDSHAKE_WANT_CONNECT 9
#define SSL_WRITE_WOULD_BLOCK     10
#define SSL_WAIT_FOR_HOOK         11

#ifndef UIO_MAXIOV
#define NET_MAX_IOV 16          // UIO_MAXIOV shall be at least 16 1003.1g (5.4.1.1)
#else
#define NET_MAX_IOV UIO_MAXIOV
#endif

ClassAllocator<SSLNetVConnection> sslNetVCAllocator("sslNetVCAllocator");

namespace {
  /// Callback to get two locks.
  /// The lock for this continuation, and for the target continuation.
  class ContWrapper : public Continuation
  {
  public:
    /** Constructor.
        This takes the secondary @a mutex and the @a target continuation
        to invoke, along with the arguments for that invocation.
    */
    ContWrapper(
      ProxyMutex* mutex ///< Mutex for this continuation (primary lock).
      , Continuation* target ///< "Real" continuation we want to call.
      , int eventId = EVENT_IMMEDIATE ///< Event ID for invocation of @a target.
      , void* edata = 0 ///< Data for invocation of @a target.
      )
      : Continuation(mutex)
      , _target(target)
      , _eventId(eventId)
      , _edata(edata)
    {
      SET_HANDLER(&ContWrapper::event_handler);
    }

    /// Required event handler method.
    int event_handler(int, void*)
    {
      EThread* eth = this_ethread();

      MUTEX_TRY_LOCK(lock, _target->mutex, eth);
      if (lock.is_locked()) { // got the target lock, we can proceed.
        _target->handleEvent(_eventId, _edata);
        delete this;
      } else { // can't get both locks, try again.
        eventProcessor.schedule_imm(this, ET_NET);
      }
      return 0;
    }

    /** Convenience static method.

        This lets a client make one call and not have to (accurately)
        copy the invocation logic embedded here. We duplicate it near
        by textually so it is easier to keep in sync.

        This takes the same arguments as the constructor but, if the
        lock can be obtained immediately, does not construct an
        instance but simply calls the @a target.
    */
    static void wrap(
      ProxyMutex* mutex ///< Mutex for this continuation (primary lock).
      , Continuation* target ///< "Real" continuation we want to call.
      , int eventId = EVENT_IMMEDIATE ///< Event ID for invocation of @a target.
      , void* edata = 0 ///< Data for invocation of @a target.
      ) {
      EThread* eth = this_ethread();
      MUTEX_TRY_LOCK(lock, target->mutex, eth);
      if (lock.is_locked()) {
        target->handleEvent(eventId, edata);
      } else {
        eventProcessor.schedule_imm(new ContWrapper(mutex, target, eventId, edata), ET_NET);
      }
    }

  private:
    Continuation* _target; ///< Continuation to invoke.
    int _eventId; ///< with this event
    void* _edata; ///< and this data
  };
}

//
// Private
//

static SSL *
make_ssl_connection(SSL_CTX * ctx, SSLNetVConnection * netvc)
{
  SSL * ssl;

  if (likely(ssl = SSL_new(ctx))) {
    netvc->ssl = ssl;

    // Only set up the bio stuff for the server side
    if (netvc->getSSLClientConnection()) {
      SSL_set_fd(ssl, netvc->get_socket());
    } else {
      netvc->initialize_handshake_buffers();
      BIO *rbio = BIO_new(BIO_s_mem());
      BIO *wbio = BIO_new_fd(netvc->get_socket(), BIO_NOCLOSE);
      BIO_set_mem_eof_return(wbio, -1); 
      SSL_set_bio(ssl, rbio, wbio);
    }

    SSL_set_app_data(ssl, netvc);
  }

  return ssl;
}

static void
debug_certificate_name(const char * msg, X509_NAME * name)
{
  BIO * bio;

  if (name == NULL) {
    return;
  }

  bio = BIO_new(BIO_s_mem());
  if (bio == NULL) {
    return;
  }

  if (X509_NAME_print_ex(bio, name, 0 /* indent */, XN_FLAG_ONELINE) > 0) {
    long len;
    char * ptr;
    len = BIO_get_mem_data(bio, &ptr);
    Debug("ssl", "%s %.*s", msg, (int)len, ptr);
  }

  BIO_free(bio);
}

static int
ssl_read_from_net(SSLNetVConnection * sslvc, EThread * lthread, int64_t &ret)
{
  NetState *s = &sslvc->read;
  MIOBufferAccessor & buf = s->vio.buffer;
  IOBufferBlock *b = buf.writer()->first_write_block();
  int event = SSL_READ_ERROR_NONE;
  int64_t bytes_read;
  int64_t block_write_avail;
  ssl_error_t sslErr = SSL_ERROR_NONE;
  int64_t nread = 0;

  for (bytes_read = 0; (b != 0) && (sslErr == SSL_ERROR_NONE); b = b->next) {
    block_write_avail = b->write_avail();

    Debug("ssl", "[SSL_NetVConnection::ssl_read_from_net] b->write_avail()=%" PRId64, block_write_avail);

    int64_t offset = 0;
    // while can be replaced with if - need to test what works faster with openssl
    while (block_write_avail > 0) {
      sslErr = SSLReadBuffer (sslvc->ssl, b->end() + offset, block_write_avail, nread);

      Debug("ssl", "[SSL_NetVConnection::ssl_read_from_net] nread=%d", (int)nread);

      switch (sslErr) {
      case SSL_ERROR_NONE:

#if DEBUG
        SSLDebugBufferPrint("ssl_buff", b->end() + offset, nread, "SSL Read");
#endif

        ink_assert(nread);

        bytes_read += nread;
        offset += nread;
        block_write_avail -= nread;
        ink_assert(block_write_avail >= 0);

        continue;

      case SSL_ERROR_WANT_WRITE:
        event = SSL_WRITE_WOULD_BLOCK;
        SSL_INCREMENT_DYN_STAT(ssl_error_want_write);
        Debug("ssl.error", "[SSL_NetVConnection::ssl_read_from_net] SSL_ERROR_WOULD_BLOCK(write)");
        break;
      case SSL_ERROR_WANT_READ:
        event = SSL_READ_WOULD_BLOCK;
        SSL_INCREMENT_DYN_STAT(ssl_error_want_read);
        Debug("ssl.error", "[SSL_NetVConnection::ssl_read_from_net] SSL_ERROR_WOULD_BLOCK(read)");
        break;
      case SSL_ERROR_WANT_X509_LOOKUP:
        event = SSL_READ_WOULD_BLOCK;
        SSL_INCREMENT_DYN_STAT(ssl_error_want_x509_lookup);
        Debug("ssl.error", "[SSL_NetVConnection::ssl_read_from_net] SSL_ERROR_WOULD_BLOCK(read/x509 lookup)");
        break;
      case SSL_ERROR_SYSCALL:
        SSL_INCREMENT_DYN_STAT(ssl_error_syscall);
        if (nread != 0) {
          // not EOF
          event = SSL_READ_ERROR;
          ret = errno;
          Debug("ssl.error", "[SSL_NetVConnection::ssl_read_from_net] SSL_ERROR_SYSCALL, underlying IO error: %s", strerror(errno));
        } else {
          // then EOF observed, treat it as EOS
          event = SSL_READ_EOS;
          //Error("[SSL_NetVConnection::ssl_read_from_net] SSL_ERROR_SYSCALL, EOF observed violating SSL protocol");
        }
        break;
      case SSL_ERROR_ZERO_RETURN:
        event = SSL_READ_EOS;
        SSL_INCREMENT_DYN_STAT(ssl_error_zero_return);
        Debug("ssl.error", "[SSL_NetVConnection::ssl_read_from_net] SSL_ERROR_ZERO_RETURN");
        break;
      case SSL_ERROR_SSL:
      default:
        event = SSL_READ_ERROR;
        ret = errno;
        SSL_CLR_ERR_INCR_DYN_STAT(ssl_error_ssl, "[SSL_NetVConnection::ssl_read_from_net]: errno=%d", errno);
        break;
      }                         // switch
      break;
    }                           // while( block_write_avail > 0 )
  }                             // for ( bytes_read = 0; (b != 0); b = b->next)

  if (bytes_read > 0) {
    Debug("ssl", "[SSL_NetVConnection::ssl_read_from_net] bytes_read=%" PRId64, bytes_read);
    buf.writer()->fill(bytes_read);
    s->vio.ndone += bytes_read;
    sslvc->netActivity(lthread);

    ret = bytes_read;

    if (s->vio.ntodo() <= 0) {
      event = SSL_READ_COMPLETE;
    } else {
      event = SSL_READ_READY;
    }
  } else                        // if( bytes_read > 0 )
  {
#if defined (_DEBUG)
    if (bytes_read == 0) {
      Debug("ssl", "[SSL_NetVConnection::ssl_read_from_net] bytes_read == 0");
    }
#endif
  }
  return (event);

}

/**
 * Read from socket directly for handshake data.  Store the data in 
 * a MIOBuffer.  Place the data in the read BIO so the openssl library
 * has access to it.  
 * If for some ready we much abort out of the handshake, we can replay
 * the stored data (e.g. back out to blind tunneling)
 */
int64_t
SSLNetVConnection::read_raw_data()
{
  int64_t r = 0;
  int64_t toread = INT_MAX;

  // read data
  int64_t rattempted = 0, total_read = 0;
  int niov = 0;
  IOVec tiovec[NET_MAX_IOV];
  if (toread) {
    IOBufferBlock *b = this->handShakeBuffer->first_write_block();
    do {
      niov = 0;
      rattempted = 0;
      while (b && niov < NET_MAX_IOV) {
        int64_t a = b->write_avail();
        if (a > 0) {
          tiovec[niov].iov_base = b->_end;
          int64_t togo = toread - total_read - rattempted;
          if (a > togo)
            a = togo;
          tiovec[niov].iov_len = a;
          rattempted += a;
          niov++;
          if (a >= togo)
            break;
        }
        b = b->next;
      }

      if (niov == 1) {
        r = socketManager.read(this->con.fd, tiovec[0].iov_base, tiovec[0].iov_len);
      } else {
        r = socketManager.readv(this->con.fd, &tiovec[0], niov);
      }
      NET_DEBUG_COUNT_DYN_STAT(net_calls_to_read_stat, 1);
      total_read += rattempted;
    } while (rattempted && r == rattempted && total_read < toread);

    // if we have already moved some bytes successfully, summarize in r
    if (total_read != rattempted) {
      if (r <= 0)
        r = total_read - rattempted;
      else
        r = total_read - rattempted + r;
    }
    // check for errors
    if (r <= 0) {

      if (r == -EAGAIN || r == -ENOTCONN) {
        NET_DEBUG_COUNT_DYN_STAT(net_calls_to_read_nodata_stat, 1);
        return r;
      }

      if (!r || r == -ECONNRESET) {
        return r;
      }
      return r;
    }
    NET_SUM_DYN_STAT(net_read_bytes_stat, r);

    this->handShakeBuffer->fill(r);

  } else
    r = 0;

  char *start = this->handShakeReader->start();
  char *end = this->handShakeReader->end();

  // Sets up the buffer as a read only bio target
  // Must be reset on each read
  BIO *rbio = BIO_new_mem_buf(start, end - start);
  BIO_set_mem_eof_return(rbio, -1); 
  // Assigning directly into the SSL structure
  // is dirty, but there is no openssl function that only
  // assigns the read bio.  Originally I was getting and
  // resetting the same write bio, but that caused the 
  // inserted buffer bios to be freed and then reinserted.
  //BIO *wbio = SSL_get_wbio(this->ssl);
  //SSL_set_bio(this->ssl, rbio, wbio);
  this->ssl->rbio = rbio;
 
  return r;
}


//changed by YTS Team, yamsat
void
SSLNetVConnection::net_read_io(NetHandler *nh, EThread *lthread)
{
  int ret;
  int64_t r = 0;
  int64_t bytes = 0;
  NetState *s = &this->read;
  MIOBufferAccessor &buf = s->vio.buffer;
  int64_t ntodo = s->vio.ntodo();

  if (HttpProxyPort::TRANSPORT_BLIND_TUNNEL == this->attributes) {
    this->super::net_read_io(nh, lthread);
    return;
  }

  if (sslClientRenegotiationAbort == true) {
    this->read.triggered = 0;
    readSignalError(nh, (int)r);
    Debug("ssl", "[SSLNetVConnection::net_read_io] client renegotiation setting read signal error");
    return;
  }

  MUTEX_TRY_LOCK_FOR(lock, s->vio.mutex, lthread, s->vio._cont);
  if (!lock.is_locked()) {
    readReschedule(nh);
    return;
  }
  // If it is not enabled, lower its priority.  This allows
  // a fast connection to speed match a slower connection by
  // shifting down in priority even if it could read.
  if (!s->enabled || s->vio.op != VIO::READ) {
    read_disable(nh, this);
    return;
  }

  ink_assert(buf.writer());

  // This function will always return true unless
  // vc is an SSLNetVConnection.
  if (!getSSLHandShakeComplete()) {
    int err;
    int data_to_read = 0;
    char *data_ptr = NULL;

    // Not done handshaking, go into the SSL handshake logic again
    if (!getSSLHandShakeComplete()) {

      if (getSSLClientConnection()) {
        ret = sslStartHandShake(SSL_EVENT_CLIENT, err);
      } else {
        ret = sslStartHandShake(SSL_EVENT_SERVER, err);
      }
      // If we have flipped to blind tunnel, don't read ahead
      if (this->handShakeReader) {
        if (this->attributes != HttpProxyPort::TRANSPORT_BLIND_TUNNEL) {
          // Check and consume data that has been read
          int data_still_to_read = BIO_get_mem_data(SSL_get_rbio(this->ssl), &data_ptr);
          data_to_read = this->handShakeReader->read_avail();
          this->handShakeReader->consume(data_to_read - data_still_to_read);
        }
        else {  // Now in blind tunnel. Set things up to read what is in the buffer
          this->readSignalDone(VC_EVENT_READ_COMPLETE, nh);
  
          // If the handshake isn't set yet, this means the tunnel 
          // decision was make in the SNI callback.  We must move
          // the client hello message back into the standard read.vio
          // so it will get forwarded onto the origin server
          if (!this->sslHandShakeComplete) {
            // Kick things to get the http forwarding buffers set up
            this->sslHandShakeComplete = 1;
            // Copy over all data already read in during the SSL_accept
            // (the client hello message)
            NetState *s = &this->read;
            MIOBufferAccessor &buf = s->vio.buffer;
            int64_t r = buf.writer()->write(this->handShakeHolder);
            s->vio.nbytes += r;
            s->vio.ndone += r;
 
            // Clean up the handshake buffers
            this->free_handshake_buffers();

            // Kick things again, so the data that was copied into the
            // vio.read buffer gets processed
            this->readSignalDone(VC_EVENT_READ_COMPLETE, nh);
          }
          return;
        }
      }

      if (ret == EVENT_ERROR) {
        this->read.triggered = 0;
        readSignalError(nh, err);
      } else if (ret == SSL_HANDSHAKE_WANT_READ || ret == SSL_HANDSHAKE_WANT_ACCEPT) {
        read.triggered = 0;
        nh->read_ready_list.remove(this);
        readReschedule(nh);
      } else if (ret == SSL_HANDSHAKE_WANT_CONNECT || ret == SSL_HANDSHAKE_WANT_WRITE) {
        write.triggered = 0;
        nh->write_ready_list.remove(this);
        writeReschedule(nh);
      } else if (ret == EVENT_DONE) {
        // If this was driven by a zero length read, signal complete when
        // the handshake is complete. Otherwise set up for continuing read
        // operations.
        if (ntodo <= 0) {
          readSignalDone(VC_EVENT_READ_COMPLETE, nh);
        } else {
          read.triggered = 1;
          if (read.enabled)
            nh->read_ready_list.in_or_enqueue(this);
        }
      } else if (ret == SSL_WAIT_FOR_HOOK) {
        // avoid readReschedule - done when the plugin calls us back to reenable
      } else {
        readReschedule(nh);
      }
    }
    return;
  }

  // If there is nothing to do or no space available, disable connection
  if (ntodo <= 0 || !buf.writer()->write_avail()) {
    read_disable(nh, this);
    return;
  }

  // At this point we are at the post-handshake SSL processing
  // If the read BIO is not already a socket, consider changing it
  if (this->handShakeReader) {
    if (this->handShakeReader->read_avail() <= 0) { 
      // Switch the read bio over to a socket bio
      SSL_set_rfd(this->ssl, this->get_socket());
      this->free_handshake_buffers();
    } 
    else { // There is still data in the buffer to drain
      char *data_ptr = NULL;
      int data_still_to_read = BIO_get_mem_data(SSL_get_rbio(this->ssl), &data_ptr);
      if (data_still_to_read >  0) {
        // Still data remaining in the current BIO block
      }
      else { 
        // reset the block
        char *start = this->handShakeReader->start();
        char *end = this->handShakeReader->end();
        // Sets up the buffer as a read only bio target
        // Must be reset on each read
        BIO *rbio = BIO_new_mem_buf(start, end - start);
        BIO_set_mem_eof_return(rbio, -1); 
        // So assigning directly into the SSL structure
        // is dirty, but there is no openssl function that only
        // assigns the read bio.  Originally I was getting and
        // resetting the same write bio, but that caused the 
        // inserted buffer bios to be freed and then reinserted.
        this->ssl->rbio = rbio;
        //BIO *wbio = SSL_get_wbio(this->ssl);
        //SSL_set_bio(this->ssl, rbio, wbio);
      }
    }
  }
  // Otherwise, we already replaced the buffer bio with a socket bio

  // not sure if this do-while loop is really needed here, please replace 
  // this comment if you know
  do {
    ret = ssl_read_from_net(this, lthread, r);
    if (ret == SSL_READ_READY || ret == SSL_READ_ERROR_NONE) {
      bytes += r;
    }
    ink_assert(bytes >= 0);
  } while ((ret == SSL_READ_READY && bytes == 0) || ret == SSL_READ_ERROR_NONE);

  if (bytes > 0) {
    if (ret == SSL_READ_WOULD_BLOCK || ret == SSL_READ_READY) {
      if (readSignalAndUpdate(VC_EVENT_READ_READY) != EVENT_CONT) {
        Debug("ssl", "ssl_read_from_net, readSignal != EVENT_CONT");
        return;
      }
    }
  }

  switch (ret) {
  case SSL_READ_ERROR_NONE:
  case SSL_READ_READY:
    readReschedule(nh);
    return;
    break;
  case SSL_WRITE_WOULD_BLOCK:
  case SSL_READ_WOULD_BLOCK:
    if (lock.get_mutex() != s->vio.mutex.m_ptr) {
      Debug("ssl", "ssl_read_from_net, mutex switched");
      if (ret == SSL_READ_WOULD_BLOCK)
        readReschedule(nh);
      else
        writeReschedule(nh);
      return;
    }
    // reset the trigger and remove from the ready queue
    // we will need to be retriggered to read from this socket again
    read.triggered = 0;
    nh->read_ready_list.remove(this);
    Debug("ssl", "read_from_net, read finished - would block");
#ifdef TS_USE_PORT
    if (ret == SSL_READ_WOULD_BLOCK)
      readReschedule(nh);
    else
      writeReschedule(nh);
#endif
    break;

  case SSL_READ_EOS:
    // close the connection if we have SSL_READ_EOS, this is the return value from ssl_read_from_net() if we get an SSL_ERROR_ZERO_RETURN from SSL_get_error()
    // SSL_ERROR_ZERO_RETURN means that the origin server closed the SSL connection
    read.triggered = 0;
    readSignalDone(VC_EVENT_EOS, nh);

    if (bytes > 0) {
      Debug("ssl", "read_from_net, read finished - EOS");
    } else {
      Debug("ssl", "read_from_net, read finished - 0 useful bytes read, bytes used by SSL layer");
    }
    break;
  case SSL_READ_COMPLETE:
    readSignalDone(VC_EVENT_READ_COMPLETE, nh);
    Debug("ssl", "read_from_net, read finished - signal done");
    break;
  case SSL_READ_ERROR:
    this->read.triggered = 0;
    readSignalError(nh, (int)r);
    Debug("ssl", "read_from_net, read finished - read error");
    break;
  }

}


int64_t
SSLNetVConnection::load_buffer_and_write(int64_t towrite, int64_t &wattempted, int64_t &total_written, MIOBufferAccessor & buf, int &needs)
{
  ProxyMutex *mutex = this_ethread()->mutex;
  int64_t r = 0;
  int64_t l = 0;
  uint32_t dynamic_tls_record_size = 0;
  ssl_error_t err = SSL_ERROR_NONE;

  // XXX Rather than dealing with the block directly, we should use the IOBufferReader API.
  int64_t offset = buf.reader()->start_offset;
  IOBufferBlock *b = buf.reader()->block;

  // Dynamic TLS record sizing
  ink_hrtime now = 0;
  if (SSLConfigParams::ssl_maxrecord == -1) {
    now = ink_get_hrtime_internal();
    int msec_since_last_write = ink_hrtime_diff_msec(now, sslLastWriteTime);

    if (msec_since_last_write > SSL_DEF_TLS_RECORD_MSEC_THRESHOLD) {
      // reset sslTotalBytesSent upon inactivity for SSL_DEF_TLS_RECORD_MSEC_THRESHOLD
      sslTotalBytesSent = 0;
    }
    Debug("ssl", "SSLNetVConnection::loadBufferAndCallWrite, now %" PRId64 ",lastwrite %" PRId64 " ,msec_since_last_write %d", now, sslLastWriteTime, msec_since_last_write);
  }

  if (HttpProxyPort::TRANSPORT_BLIND_TUNNEL == this->attributes) {
    return this->super::load_buffer_and_write(towrite, wattempted, total_written, buf, needs);
  }

  do {
    // check if we have done this block
    l = b->read_avail();
    l -= offset;
    if (l <= 0) {
      offset = -l;
      b = b->next;
      continue;
    }
    // check if to amount to write exceeds that in this buffer
    int64_t wavail = towrite - total_written;

    if (l > wavail) {
      l = wavail;
    }

    // TS-2365: If the SSL max record size is set and we have
    // more data than that, break this into smaller write
    // operations.
    int64_t orig_l = l;
    if (SSLConfigParams::ssl_maxrecord > 0 && l > SSLConfigParams::ssl_maxrecord) {
      l = SSLConfigParams::ssl_maxrecord;
    } else if (SSLConfigParams::ssl_maxrecord == -1) {
      if (sslTotalBytesSent < SSL_DEF_TLS_RECORD_BYTE_THRESHOLD) {
        dynamic_tls_record_size = SSL_DEF_TLS_RECORD_SIZE;
        SSL_INCREMENT_DYN_STAT(ssl_total_dyn_def_tls_record_count);
      } else {
        dynamic_tls_record_size = SSL_MAX_TLS_RECORD_SIZE;
        SSL_INCREMENT_DYN_STAT(ssl_total_dyn_max_tls_record_count);
      }
      if (l > dynamic_tls_record_size) {
        l = dynamic_tls_record_size;
      }
    }

    if (!l) {
      break;
    }

    wattempted = l;
    total_written += l;
    Debug("ssl", "SSLNetVConnection::loadBufferAndCallWrite, before SSLWriteBuffer, l=%" PRId64", towrite=%" PRId64", b=%p",
          l, towrite, b);
    err = SSLWriteBuffer(ssl, b->start() + offset, l, r);

    if (r == l) {
      wattempted = total_written;
    }
    if (l == orig_l) {
        // on to the next block
        offset = 0;
        b = b->next;
    } else {
        offset += l;
    }

    Debug("ssl", "SSLNetVConnection::loadBufferAndCallWrite,Number of bytes written=%" PRId64" , total=%" PRId64"", r, total_written);
    NET_DEBUG_COUNT_DYN_STAT(net_calls_to_write_stat, 1);
  } while (r == l && total_written < towrite && b);

  if (r > 0) {
    sslLastWriteTime = now;
    sslTotalBytesSent += total_written;
    if (total_written != wattempted) {
      Debug("ssl", "SSLNetVConnection::loadBufferAndCallWrite, wrote some bytes, but not all requested.");
      // I'm not sure how this could happen. We should have tried and hit an EAGAIN.
      needs |= EVENTIO_WRITE;
      return (r);
    } else {
      Debug("ssl", "SSLNetVConnection::loadBufferAndCallWrite, write successful.");
      return (total_written);
    }
  } else {
    switch (err) {
    case SSL_ERROR_NONE:
      Debug("ssl", "SSL_write-SSL_ERROR_NONE");
      break;
    case SSL_ERROR_WANT_READ:
      needs |= EVENTIO_READ;
      r = -EAGAIN;
      SSL_INCREMENT_DYN_STAT(ssl_error_want_read);
      Debug("ssl.error", "SSL_write-SSL_ERROR_WANT_READ");
      break;
    case SSL_ERROR_WANT_WRITE:
    case SSL_ERROR_WANT_X509_LOOKUP: {
      if (SSL_ERROR_WANT_WRITE == err)
        SSL_INCREMENT_DYN_STAT(ssl_error_want_write);
      else if (SSL_ERROR_WANT_X509_LOOKUP == err)
        SSL_INCREMENT_DYN_STAT(ssl_error_want_x509_lookup);

      needs |= EVENTIO_WRITE;
      r = -EAGAIN;
      Debug("ssl.error", "SSL_write-SSL_ERROR_WANT_WRITE");
      break;
    }
    case SSL_ERROR_SYSCALL:
      r = -errno;
      SSL_INCREMENT_DYN_STAT(ssl_error_syscall);
      Debug("ssl.error", "SSL_write-SSL_ERROR_SYSCALL");
      break;
      // end of stream
    case SSL_ERROR_ZERO_RETURN:
      r = -errno;
      SSL_INCREMENT_DYN_STAT(ssl_error_zero_return);
      Debug("ssl.error", "SSL_write-SSL_ERROR_ZERO_RETURN");
      break;
    case SSL_ERROR_SSL:
    default:
      r = -errno;
      SSL_CLR_ERR_INCR_DYN_STAT(ssl_error_ssl, "SSL_write-SSL_ERROR_SSL errno=%d", errno);
      break;
    }
    return (r);
  }
}

SSLNetVConnection::SSLNetVConnection():
  ssl(NULL),
  sslHandshakeBeginTime(0),
  sslLastWriteTime(0),
  sslTotalBytesSent(0),
  hookOpRequested(TS_SSL_HOOK_OP_DEFAULT),
  sslHandShakeComplete(false),
  sslClientConnection(false),
  sslClientRenegotiationAbort(false),
  handShakeBuffer(NULL),
  handShakeHolder(NULL),
  handShakeReader(NULL),
  sslPreAcceptHookState(SSL_HOOKS_INIT),
  sslSNIHookState(SNI_HOOKS_INIT),
  npnSet(NULL),
  npnEndpoint(NULL)
{
}

void
SSLNetVConnection::free(EThread * t) {
  NET_SUM_GLOBAL_DYN_STAT(net_connections_currently_open_stat, -1);
  got_remote_addr = 0;
  got_local_addr = 0;
  read.vio.mutex.clear();
  write.vio.mutex.clear();
  this->mutex.clear();
  flags = 0;
  SET_CONTINUATION_HANDLER(this, (SSLNetVConnHandler) & SSLNetVConnection::startEvent);
  nh = NULL;
  read.triggered = 0;
  write.triggered = 0;
  options.reset();
  closed = 0;
  ink_assert(con.fd == NO_FD);
  if (ssl != NULL) {
    /*if (sslHandShakeComplete)
       SSL_set_shutdown(ssl, SSL_SENT_SHUTDOWN|SSL_RECEIVED_SHUTDOWN); */
    SSL_free(ssl);
    ssl = NULL;
  }
  sslHandShakeComplete = false;
  sslClientConnection = false;
  sslLastWriteTime = 0;
  sslTotalBytesSent = 0;
  sslClientRenegotiationAbort = false;
  if (SSL_HOOKS_ACTIVE == sslPreAcceptHookState) {
    Error("SSLNetVconnection freed with outstanding hook");
  }
  sslPreAcceptHookState = SSL_HOOKS_INIT;
  curHook = 0;
  hookOpRequested = TS_SSL_HOOK_OP_DEFAULT;
  npnSet = NULL;
  npnEndpoint= NULL;

  if (from_accept_thread) {
    sslNetVCAllocator.free(this);  
  } else {
    THREAD_FREE(this, sslNetVCAllocator, t);
  }
}

int
SSLNetVConnection::sslStartHandShake(int event, int &err)
{

  switch (event) {
  case SSL_EVENT_SERVER:
    if (this->ssl == NULL) {
      SSLCertificateConfig::scoped_config lookup;
      IpEndpoint ip;
      int namelen = sizeof(ip);
      safe_getsockname(this->get_socket(), &ip.sa, &namelen);
      SSLCertContext *cc = lookup->find(ip);
      if (is_debug_tag_set("ssl")) {
        IpEndpoint src, dst;
        ip_port_text_buffer ipb1, ipb2;
        int ip_len;
        
        safe_getsockname(this->get_socket(), &dst.sa, &(ip_len = sizeof ip));
        safe_getpeername(this->get_socket(), &src.sa, &(ip_len = sizeof ip));
        ats_ip_nptop(&dst, ipb1, sizeof(ipb1));
        ats_ip_nptop(&src, ipb2, sizeof(ipb2));
        Debug("ssl", "IP context is %p for [%s] -> [%s], default context %p", cc, ipb2, ipb1, lookup->defaultContext());
      }

      // Escape if this is marked to be a tunnel.
      // No data has been read at this point, so we can go
      // directly into blind tunnel mode
      if (cc && SSLCertContext::OPT_TUNNEL == cc->opt && this->is_transparent) {
        this->attributes = HttpProxyPort::TRANSPORT_BLIND_TUNNEL;
        sslHandShakeComplete = 1;
        SSL_free(this->ssl);
        this->ssl = NULL;
        return EVENT_DONE;
      }
 

      // Attach the default SSL_CTX to this SSL session. The default context is never going to be able
      // to negotiate a SSL session, but it's enough to trampoline us into the SNI callback where we
      // can select the right server certificate.
      this->ssl = make_ssl_connection(lookup->defaultContext(), this);
    }

    if (this->ssl == NULL) {
      SSLErrorVC(this, "failed to create SSL server session");
      return EVENT_ERROR;
    }

    return sslServerHandShakeEvent(err);

  case SSL_EVENT_CLIENT:
    if (this->ssl == NULL) {
      this->ssl = make_ssl_connection(ssl_NetProcessor.client_ctx, this);
    }

    if (this->ssl == NULL) {
      SSLErrorVC(this, "failed to create SSL client session");
      return EVENT_ERROR;
    }

    return sslClientHandShakeEvent(err);

  default:
    ink_assert(0);
    return EVENT_ERROR;
  }

}

int
SSLNetVConnection::sslServerHandShakeEvent(int &err)
{
  if (SSL_HOOKS_DONE != sslPreAcceptHookState) {
    // Get the first hook if we haven't started invoking yet.
    if (SSL_HOOKS_INIT == sslPreAcceptHookState) {
      curHook = ssl_hooks->get(TS_VCONN_PRE_ACCEPT_INTERNAL_HOOK);
      sslPreAcceptHookState = SSL_HOOKS_INVOKE;
    } else if (SSL_HOOKS_INVOKE == sslPreAcceptHookState) {
      // if the state is anything else, we haven't finished
      // the previous hook yet.
      curHook = curHook->next();
    }
    if (SSL_HOOKS_INVOKE == sslPreAcceptHookState) {
      if (0 == curHook) { // no hooks left, we're done
        sslPreAcceptHookState = SSL_HOOKS_DONE;
      } else {
        sslPreAcceptHookState = SSL_HOOKS_ACTIVE;
        ContWrapper::wrap(mutex, curHook->m_cont, TS_EVENT_VCONN_PRE_ACCEPT, this);
        return SSL_WAIT_FOR_HOOK;
      }
    } else { // waiting for hook to complete
      /* A note on waiting for the hook. I believe that because this logic
         cannot proceed as long as a hook is outstanding, the underlying VC
         can't go stale. If that can happen for some reason, we'll need to be
         more clever and provide some sort of cancel mechanism. I have a trap
         in SSLNetVConnection::free to check for this.
      */
      return SSL_WAIT_FOR_HOOK;
    }
  }

  // If a blind tunnel was requested in the pre-accept calls, convert. 
  // Again no data has been exchanged, so we can go directly
  // without data replay.
  // Note we can't arrive here if a hook is active.
  if (TS_SSL_HOOK_OP_TUNNEL == hookOpRequested) {
    this->attributes = HttpProxyPort::TRANSPORT_BLIND_TUNNEL;
    SSL_free(this->ssl);
    this->ssl = NULL;
    sslHandShakeComplete = 1;
    return EVENT_DONE;
  } else if (TS_SSL_HOOK_OP_TERMINATE == hookOpRequested) {
    sslHandShakeComplete = 1;
    return EVENT_DONE;
  }

  // All the pre-accept hooks have completed, proceed with the actual accept.

  char *data_ptr = NULL;
  int data_to_read = BIO_get_mem_data(SSL_get_rbio(this->ssl), &data_ptr);
  if (data_to_read <= 0) { // If there is not already data in the buffer
    // Read from socket to fill in the BIO buffer with the 
    // raw handshake data before calling the ssl accept calls.
    int64_t data_read;
    if ((data_read = this->read_raw_data()) > 0) {
      BIO_get_mem_data(SSL_get_rbio(this->ssl), &data_ptr);
    }
  }

  ssl_error_t ssl_error = SSLAccept(ssl);

  if (ssl_error != SSL_ERROR_NONE) {
    err = errno;
    SSLDebugVC(this,"SSL handshake error: %s (%d), errno=%d", SSLErrorName(ssl_error), ssl_error, err);
  }

  switch (ssl_error) {
  case SSL_ERROR_NONE:
    if (is_debug_tag_set("ssl")) {
      X509 * cert = SSL_get_peer_certificate(ssl);

      Debug("ssl", "SSL server handshake completed successfully");
      if (cert) {
        debug_certificate_name("client certificate subject CN is", X509_get_subject_name(cert));
        debug_certificate_name("client certificate issuer CN is", X509_get_issuer_name(cert));
        X509_free(cert);
      }
    }

    sslHandShakeComplete = true;

    if (sslHandshakeBeginTime) {
      const ink_hrtime ssl_handshake_time = ink_get_hrtime() - sslHandshakeBeginTime;
      Debug("ssl", "ssl handshake time:%" PRId64, ssl_handshake_time);
      sslHandshakeBeginTime = 0;
      SSL_INCREMENT_DYN_STAT_EX(ssl_total_handshake_time_stat, ssl_handshake_time);
      SSL_INCREMENT_DYN_STAT(ssl_total_success_handshake_count_stat);
    }

    {
      const unsigned char * proto = NULL;
      unsigned len = 0;

      // If it's possible to negotiate both NPN and ALPN, then ALPN
      // is preferred since it is the server's preference.  The server
      // preference would not be meaningful if we let the client
      // preference have priority.

#if TS_USE_TLS_ALPN
      SSL_get0_alpn_selected(ssl, &proto, &len);
#endif /* TS_USE_TLS_ALPN */

#if TS_USE_TLS_NPN
      if (len == 0) {
        SSL_get0_next_proto_negotiated(ssl, &proto, &len);
      }
#endif /* TS_USE_TLS_NPN */

      if (len) {
        // If there's no NPN set, we should not have done this negotiation.
        ink_assert(this->npnSet != NULL);

        this->npnEndpoint = this->npnSet->findEndpoint(proto, len);
        this->npnSet = NULL;

        if (this->npnEndpoint == NULL) {
          Error("failed to find registered SSL endpoint for '%.*s'", (int)len, (const char *)proto);
          return EVENT_ERROR;
        }

        Debug("ssl", "client selected next protocol '%.*s'", len, proto);
      } else {
        Debug("ssl", "client did not select a next protocol");
      }
    }

    return EVENT_DONE;

  case SSL_ERROR_WANT_CONNECT:
    return SSL_HANDSHAKE_WANT_CONNECT;

  case SSL_ERROR_WANT_WRITE:
    return SSL_HANDSHAKE_WANT_WRITE;

  case SSL_ERROR_WANT_READ:
    return SSL_HANDSHAKE_WANT_READ;

// This value is only defined in openssl has been patched to
// enable the sni callback to break out of the SSL_accept processing
#ifdef SSL_ERROR_WANT_SNI_RESOLVE
  case SSL_ERROR_WANT_SNI_RESOLVE:
    if (this->attributes == HttpProxyPort::TRANSPORT_BLIND_TUNNEL ||
        TS_SSL_HOOK_OP_TUNNEL == hookOpRequested) {
      this->attributes = HttpProxyPort::TRANSPORT_BLIND_TUNNEL;
      sslHandShakeComplete = 0;
      return EVENT_CONT;
    }
    else {
      //  Stopping for some other reason, perhaps loading certificate
      //;return EVENT_ERROR;
      return EVENT_CONT;
    }
#endif

  case SSL_ERROR_WANT_ACCEPT:
  case SSL_ERROR_WANT_X509_LOOKUP:
    return EVENT_CONT;

  case SSL_ERROR_SSL:
    SSL_CLR_ERR_INCR_DYN_STAT(ssl_error_ssl, "SSLNetVConnection::sslServerHandShakeEvent, SSL_ERROR_SSL errno=%d", errno);
    // fall through
  case SSL_ERROR_ZERO_RETURN:
  case SSL_ERROR_SYSCALL:
  default:
    return EVENT_ERROR;
  }

}


int
SSLNetVConnection::sslClientHandShakeEvent(int &err)
{
#if TS_USE_TLS_SNI
  if (options.sni_servername) {
    if (SSL_set_tlsext_host_name(ssl, options.sni_servername)) {
      Debug("ssl", "using SNI name '%s' for client handshake", options.sni_servername.get());
    } else {
      Debug("ssl.error","failed to set SNI name '%s' for client handshake", options.sni_servername.get());
      SSL_INCREMENT_DYN_STAT(ssl_sni_name_set_failure);
    }
  }
#endif
  
  ssl_error_t ssl_error = SSLConnect(ssl);
  switch (ssl_error) {
  case SSL_ERROR_NONE:
    if (is_debug_tag_set("ssl")) {
      X509 * cert = SSL_get_peer_certificate(ssl);

      Debug("ssl", "SSL client handshake completed successfully");
      // if the handshake is complete and write is enabled reschedule the write
      if (closed == 0 && write.enabled)
        writeReschedule(nh);
      if (cert) {
        debug_certificate_name("server certificate subject CN is", X509_get_subject_name(cert));
        debug_certificate_name("server certificate issuer CN is", X509_get_issuer_name(cert));
        X509_free(cert);
      }
    }

    sslHandShakeComplete = true;
    return EVENT_DONE;

  case SSL_ERROR_WANT_WRITE:
    Debug("ssl.error", "SSLNetVConnection::sslClientHandShakeEvent, SSL_ERROR_WANT_WRITE");
    SSL_INCREMENT_DYN_STAT(ssl_error_want_write);
    return SSL_HANDSHAKE_WANT_WRITE;

  case SSL_ERROR_WANT_READ:
    SSL_INCREMENT_DYN_STAT(ssl_error_want_read);
    Debug("ssl.error", "SSLNetVConnection::sslClientHandShakeEvent, SSL_ERROR_WANT_READ");
    return SSL_HANDSHAKE_WANT_READ;

  case SSL_ERROR_WANT_X509_LOOKUP:
    SSL_INCREMENT_DYN_STAT(ssl_error_want_x509_lookup);
    Debug("ssl.error", "SSLNetVConnection::sslClientHandShakeEvent, SSL_ERROR_WANT_X509_LOOKUP");
    break;

  case SSL_ERROR_WANT_ACCEPT:
    return SSL_HANDSHAKE_WANT_ACCEPT;

  case SSL_ERROR_WANT_CONNECT:
    break;

  case SSL_ERROR_ZERO_RETURN:
    SSL_INCREMENT_DYN_STAT(ssl_error_zero_return);
    Debug("ssl.error", "SSLNetVConnection::sslClientHandShakeEvent, EOS");
    return EVENT_ERROR;

  case SSL_ERROR_SYSCALL:
    err = errno;
    SSL_INCREMENT_DYN_STAT(ssl_error_syscall);
    Debug("ssl.error", "SSLNetVConnection::sslClientHandShakeEvent, syscall");
    return EVENT_ERROR;
    break;


  case SSL_ERROR_SSL:
  default:
    err = errno;
    SSL_CLR_ERR_INCR_DYN_STAT(ssl_error_ssl, "SSLNetVConnection::sslClientHandShakeEvent, SSL_ERROR_SSL errno=%d", errno);
    return EVENT_ERROR;
    break;

  }
  return EVENT_CONT;

}

void
SSLNetVConnection::registerNextProtocolSet(const SSLNextProtocolSet * s)
{
  ink_release_assert(this->npnSet == NULL);
  this->npnSet = s;
}

// NextProtocolNegotiation TLS extension callback. The NPN extension
// allows the client to select a preferred protocol, so all we have
// to do here is tell them what out protocol set is.
int
SSLNetVConnection::advertise_next_protocol(SSL *ssl, const unsigned char **out, unsigned int *outlen,
                                           void * /*arg ATS_UNUSED */)
{
  SSLNetVConnection * netvc = (SSLNetVConnection *)SSL_get_app_data(ssl);

  ink_release_assert(netvc != NULL);

  if (netvc->npnSet && netvc->npnSet->advertiseProtocols(out, outlen)) {
    // Successful return tells OpenSSL to advertise.
    return SSL_TLSEXT_ERR_OK;
  }

  return SSL_TLSEXT_ERR_NOACK;
}

// ALPN TLS extension callback. Given the client's set of offered
// protocols, we have to select a protocol to use for this session.
int
SSLNetVConnection::select_next_protocol(SSL * ssl, const unsigned char ** out, unsigned char * outlen, const unsigned char * in ATS_UNUSED, unsigned inlen ATS_UNUSED, void *)
{
  SSLNetVConnection * netvc = (SSLNetVConnection *)SSL_get_app_data(ssl);
  const unsigned char * npn = NULL;
  unsigned npnsz = 0;

  ink_release_assert(netvc != NULL);

  if (netvc->npnSet && netvc->npnSet->advertiseProtocols(&npn, &npnsz)) {
    // SSL_select_next_proto chooses the first server-offered protocol that appears in the clients protocol set, ie. the
    // server selects the protocol. This is a n^2 search, so it's preferable to keep the protocol set short.

#if HAVE_SSL_SELECT_NEXT_PROTO
    if (SSL_select_next_proto((unsigned char **)out, outlen, npn, npnsz, in, inlen) == OPENSSL_NPN_NEGOTIATED) {
      Debug("ssl", "selected ALPN protocol %.*s", (int)(*outlen), *out);
      return SSL_TLSEXT_ERR_OK;
    }
#endif /* HAVE_SSL_SELECT_NEXT_PROTO */
  }

  *out = NULL;
  *outlen = 0;
  return SSL_TLSEXT_ERR_NOACK;
}

void
SSLNetVConnection::reenable(NetHandler* nh) {
  if (this->sslPreAcceptHookState != SSL_HOOKS_DONE) {
    this->sslPreAcceptHookState = SSL_HOOKS_INVOKE;
    this->readReschedule(nh);
  } else {
    // Reenabling from the SNI callback
    this->sslSNIHookState = SNI_HOOKS_CONTINUE;
  }
}


bool
SSLNetVConnection::sslContextSet(void* ctx) {
#if TS_USE_TLS_SNI
  bool zret = true;
  if (ssl)
    SSL_set_SSL_CTX(ssl, static_cast<SSL_CTX*>(ctx));
  else
    zret = false;
#else
  bool zret = false;
#endif
  return zret;
}

bool 
SSLNetVConnection::callHooks(TSHttpHookID eventId) 
{
  // Only dealing with the SNI hook so far
  ink_assert(eventId == TS_SSL_SNI_HOOK);

  APIHook *hook = ssl_hooks->get(TS_SSL_SNI_INTERNAL_HOOK);
  bool reenabled = true;
  while (hook && reenabled) {
    // Must reset to a completed state for each invocation
    this->sslSNIHookState = SNI_HOOKS_DONE;

    // Invoke the hook
    hook->invoke(TS_SSL_SNI_HOOK, this);

    // If it did not re-enable, return the code to 
    // stop the accept processing
    if (this->sslSNIHookState == SNI_HOOKS_DONE) {
      reenabled = false;
    }

    // Otherwise, look for the next plugin code
    hook = hook->next();
  }
  return reenabled;
}


