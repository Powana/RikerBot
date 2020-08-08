#include <iostream>
#include <algorithm>
#include <functional>
#include <botan/system_rng.h>
#include <botan/pubkey.h>
#include <botan/data_src.h>
#include <botan/x509_key.h>
#include <boost/log/trivial.hpp>

#include "datautils.hpp"
#include "io_core.hpp"

namespace rkr {

IOCore::IOCore(PluginLoader& ploader, bool ownership) :
    PluginBase("rkr::IOCore *"), state(mcd::HANDSHAKING), kill(0),
    compressed(false), encrypted(false), sock(ctx), rslv(ctx),
    in_is(&in_buf) {

  in_is.exceptions(in_is.eofbit | in_is.badbit | in_is.failbit);

  encryptor = Botan::Cipher_Mode::create("AES-128/CFB", Botan::ENCRYPTION);
  decryptor = Botan::Cipher_Mode::create("AES-128/CFB", Botan::DECRYPTION);
  compressor = Botan::Compression_Algorithm::create("zlib");
  decompressor = Botan::Decompression_Algorithm::create("zlib");


  ploader.provide("IO", this, ownership);
  ev = static_cast<EventCore*>(ploader.require("Event"));
  connect_event = ev->register_event("io_connect");
  kill_event = ev->register_event("kill");

  ev->register_callback("ClientboundEncryptionBegin",
      [&](EventCore::ev_id_type ev_id, const void* data) {
      encryption_begin_handler(ev_id, data);});

  ev->register_callback("ServerboundEncryptionBegin",
      [&](EventCore::ev_id_type ev_id, const void* data) {
      enable_encryption(ev_id, data);});

  ev->register_callback("ClientboundCompress",
      [&](EventCore::ev_id_type ev_id, const void* data) {
      enable_compression(ev_id, data);});

  for(int state_itr = 0; state_itr < mcd::STATE_MAX; state_itr++) {
    for(int dir_itr = 0; dir_itr < mcd::DIRECTION_MAX; dir_itr++) {
      for(int id = 0; id < mcd::protocol_max_ids[state_itr][dir_itr]; id++) {
        auto name = mcd::protocol_cstrings[state_itr][dir_itr][id];
        auto event_id = ev->register_event(name);
        packet_event_ids[state_itr][dir_itr].push_back(event_id);
      }
    }
  }
}

void IOCore::run() {
  while(!kill) {
    ctx.run();
    if(out_buf.size())
      net::async_write(sock, out_buf.data(), [&](const sys::error_code& ec,
          std::size_t len) {write_handler(ec, len);});
    // 5 bytes is the maximum size of the packet length header, but it's
    // possible for an entire packet to be shorter than that. So we prepare
    // a 5 byte buffer then use read_some to read however many bytes come.
    read_buf = in_buf.prepare(5);
    sock.async_read_some(read_buf, [&](const sys::error_code& ec,
        std::size_t len) {header_handler(ec, len);});
  }
  ev->emit(kill_event);
}

void IOCore::encode_packet(const mcd::Packet& packet) {
  static std::ostream pak_os(&pak_buf);
  static std::ostream out_os(&out_buf);

  static boost::asio::streambuf header_buf;
  static std::ostream header_os(&header_buf);

  mcd::enc_varint(pak_os, packet.packet_id);
  packet.encode(pak_os);
  auto packet_size = pak_buf.size();
  std::size_t header_size = 0;
  std::size_t write_size = 0;
  boost::asio::mutable_buffer write_buf;

  if(compressed && packet_size > threshold) {
    auto sec_vec = Botan::secure_vector<uint8_t>(packet_size);
    std::memcpy(sec_vec.data(), pak_buf.data().data(), packet_size);
    compressor->update(sec_vec, 0, true);
    auto total_size = sec_vec.size() + mcd::size_varint(packet_size);
    mcd::enc_varint(header_os, total_size);
    mcd::enc_varint(header_os, packet_size);
    header_size = header_buf.size();
    write_size = header_size + sec_vec.size();
    write_buf = out_buf.prepare(write_size);
    std::memcpy(write_buf.data(), header_buf.data().data(), header_size);
    write_buf += header_size;
    std::memcpy(write_buf.data(), sec_vec.data(), sec_vec.size());
  } else {
    if(compressed) {
      mcd::enc_varint(header_os, packet_size + 1);
      mcd::enc_byte(header_os, 0);
    } else {
      mcd::enc_varint(header_os, packet_size);
    }
    header_size = header_buf.size();
    write_size = packet_size + header_size;
    write_buf = out_buf.prepare(write_size);
    std::memcpy(write_buf.data(), header_buf.data().data(), header_size);
    write_buf += header_size;
    std::memcpy(write_buf.data(), pak_buf.data().data(), packet_size);
  }
  if(encrypted)
    encryptor->process(static_cast<std::uint8_t*>(write_buf.data()),
        write_size);
  out_buf.commit(write_size);
  header_buf.consume(header_size);
  pak_buf.consume(packet_size);

  ev->emit(packet_event_ids[state][mcd::SERVERBOUND][packet.packet_id],
      static_cast<const void*>(&packet), "mcd::" + packet.name + " *");
}

void IOCore::connect(const std::string& host, const std::string& service) {
  auto endpoints = rslv.resolve(host, service);
  net::async_connect(sock, endpoints, [&](const sys::error_code& ec,
      const ip::tcp::endpoint& ep) {connect_handler(ec, ep);});
}

void IOCore::connect_handler(const sys::error_code& ec,
    const ip::tcp::endpoint& ep) {
  if(ec.failed()) {
    BOOST_LOG_TRIVIAL(fatal) << ec.message();
    exit(-1);
  }

  compressed = false;
  encrypted = false;
  ConnectData data {ep.address().to_string(), ep.port()};
  ev->emit(connect_event, static_cast<const void*>(&data),
      "rkr::ConnectData *");
}

void IOCore::write_handler(const sys::error_code& ec, std::size_t len) {
  if(ec.failed()) {
    BOOST_LOG_TRIVIAL(fatal) << ec.message();
    exit(-1);
  }
  out_buf.consume(len);
}

void IOCore::header_handler(const sys::error_code& ec, std::size_t len) {
  if(ec.failed()) {
    BOOST_LOG_TRIVIAL(fatal) << ec.message();
    exit(-1);
  }
  if(encrypted)
    decryptor->process(static_cast<std::uint8_t*>(read_buf.data()), len);
  in_buf.commit(len);

  int varnum = mcd::verify_varint(
      static_cast<const char *>(in_buf.data().data()), in_buf.size());
  if(varnum == mcd::VARNUM_INVALID) {
    exit(-1);
  } else if (varnum == mcd::VARNUM_OVERRUN) {
    sock.async_read_some(in_buf.prepare(5 - in_buf.size()),
        [&](const sys::error_code& ec, std::size_t len)
        {header_handler(ec, len);});
  } else {
    auto varint = mcd::dec_varint(in_is);
    read_buf = in_buf.prepare(varint - in_buf.size());
    net::async_read(sock, read_buf, [&](const sys::error_code& ec,
        std::size_t len) {read_packet_handler(ec, len);});
  }
}

void IOCore::read_packet_handler(const sys::error_code& ec, std::size_t len) {
  if(ec.failed()) {
    BOOST_LOG_TRIVIAL(fatal) << ec.message();
    exit(-1);
  }
  if(encrypted)
    decryptor->process(static_cast<std::uint8_t*>(read_buf.data()), len);
  in_buf.commit(len);
  if(!compressed) {
    std::int64_t packet_id;
    try {packet_id = mcd::dec_varint(in_is);}
    catch(std::exception&) {
      BOOST_LOG_TRIVIAL(fatal) << "Failed to decode packet id";
      exit(-1);
    }
    auto packet = mcd::make_packet(state, mcd::CLIENTBOUND, packet_id);
    try {packet->decode(in_is);}
    catch(std::exception&) {
      BOOST_LOG_TRIVIAL(fatal) << "Failed to decode packet, suspect id: "
          << packet_id;
      exit(-1);
    }

    ev->emit(packet_event_ids[state][mcd::CLIENTBOUND][packet->packet_id],
        static_cast<const void*>(packet.get()), "mcd::" + packet->name + " *");
  } else {
    throw std::runtime_error("Compression Unimplemented");
  }
}

void IOCore::encryption_begin_handler(EventCore::ev_id_type ev_id,
    const void* data) {
  auto packet = static_cast<const mcd::ClientboundEncryptionBegin*>(data);
  auto mem = Botan::DataSource_Memory(const_cast<std::uint8_t*>(
      reinterpret_cast<const std::uint8_t*>(
      packet->publicKey.data())), packet->publicKey.size());
  auto key = Botan::X509::load_key(mem);
  auto& rng = Botan::system_rng();
  auto enc = Botan::PK_Encryptor_EME(*key, rng, "EME-PKCS1-v1_5");
  rng.randomize(shared_secret, std::size(shared_secret));
  auto result = enc.encrypt(shared_secret, std::size(shared_secret), rng);
  auto resp = mcd::ServerboundEncryptionBegin();
  resp.sharedSecret.resize(result.size());
  std::memcpy(resp.sharedSecret.data(), result.data(), result.size());
  result = enc.encrypt(reinterpret_cast<const std::uint8_t*>(
      packet->verifyToken.data()), packet->verifyToken.size(), rng);
  resp.verifyToken.resize(result.size());
  std::memcpy(resp.verifyToken.data(), result.data(), result.size());
  encode_packet(resp);
  delete key;
}

void IOCore::enable_encryption(EventCore::ev_id_type ev_id, const void* data) {
  encryptor->clear();
  encryptor->set_key(shared_secret, std::size(shared_secret));
  encryptor->start(shared_secret, std::size(shared_secret));
  decryptor->clear();
  decryptor->set_key(shared_secret, std::size(shared_secret));
  decryptor->start(shared_secret, std::size(shared_secret));
  encrypted = true;
}

void IOCore::enable_compression(EventCore::ev_id_type ev_id,
    const void* data) {
  auto packet = static_cast<const mcd::ClientboundCompress*>(data);
  threshold = packet->threshold;

  compressor->clear();
  compressor->start(1);
  decompressor->clear();
  decompressor->start();
}

} // namespace rkr
