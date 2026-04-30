// ============================================================================
// moki_mesh.cpp — MeshCore integration (M7)
// ============================================================================
// Wraps MeshCore's BaseChatMesh with our own minimal MyMesh subclass.
// Bridges incoming channel messages into the existing g_lora_msgs ring buffer
// so the existing chat-detail screen keeps working unchanged.
//
// What we DO:
//   * Identity from the 32-byte g_identity_secret (Stage 3c).
//   * One default channel "moki" with a user-chosen PSK (settings).
//   * Optional second channel "rn-mesh-public" with the known public PSK,
//     so we can listen-in to (and post to) the existing Heidelberg mesh.
//   * Receive: every channel message → push into g_lora_msgs ring buffer.
//   * Send: moki_mesh_send_channel(text) → broadcasts to current channel.
//
// What we DO NOT (yet):
//   * Direct messages / contact list (M4 territory)
//   * Adverts (we lurk for now; adverts come when M4 lands)
//   * Path-aware routing (BaseChatMesh handles flooding for us)

#include <Arduino.h>
#include <Mesh.h>
#include <Identity.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/ChannelDetails.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>

extern WRAPPER_CLASS radio_driver;

// Bridge into Moki's existing ring buffer (defined in main.cpp).
extern "C" void moki_lora_push_msg_external(const char *from, const char *text, int16_t rssi);

// Identity secret from Stage 3c (in main.cpp).
extern uint8_t g_identity_secret[32];

// Settings exposed by main.cpp (extern so we can read channel name + PSK).
struct moki_settings_t;
extern struct moki_settings_t g_settings;
// Direct field accessors via extern "C" wrappers in main.cpp (avoids needing
// the full struct layout here — keeps moki_mesh.cpp decoupled).
extern "C" const char *moki_settings_get_channel_name();
extern "C" const char *moki_settings_get_channel_psk();

// ── Helpers ─────────────────────────────────────────────────────────────────

// Tiny RTCClock that just tracks seconds-since-epoch using millis() offset.
// Real PCF85063 integration via SensorLib happens through main.cpp; here we
// just need *some* monotonic clock for the mesh protocol's timestamping.
class SimpleRTCClock : public mesh::RTCClock {
  uint32_t _epoch_at_boot = 1714000000;  // 2024-04-25 — sane default
public:
  uint32_t getCurrentTime() override {
    return _epoch_at_boot + (millis() / 1000);
  }
  void setCurrentTime(uint32_t time) override {
    _epoch_at_boot = (time > millis() / 1000) ? (time - millis() / 1000) : 0;
  }
};

// Deterministic RNG used ONCE during identity creation: feeds our 32-byte
// secret on the first call, then expands via SHA256 chains for any further
// bytes requested. Never used after begin() — runtime RNG is StdRNG.
class SeedRNG : public mesh::RNG {
  const uint8_t *_seed;
  uint8_t        _state[32];
  bool           _seeded = false;
public:
  SeedRNG(const uint8_t *seed) : _seed(seed) {}
  void random(uint8_t *dest, size_t sz) override {
    if (!_seeded) {
      memcpy(_state, _seed, 32);
      _seeded = true;
    }
    while (sz > 0) {
      size_t chunk = sz > 32 ? 32 : sz;
      memcpy(dest, _state, chunk);
      dest += chunk; sz -= chunk;
      if (sz > 0) {
        // Hash-chain advance: state := SHA256(state)
        mesh::Utils::sha256(_state, 32, _state, 32);
      }
    }
  }
};

// Send timeout factors borrowed from simple_secure_chat.
#define SEND_TIMEOUT_BASE_MILLIS          500
#define FLOOD_SEND_TIMEOUT_FACTOR         16.0f
#define DIRECT_SEND_PERHOP_FACTOR         6.0f
#define DIRECT_SEND_PERHOP_EXTRA_MILLIS   250

// ── MyMesh ──────────────────────────────────────────────────────────────────

class MyMesh : public BaseChatMesh, ContactVisitor {
  ChannelDetails* _moki_channel  = NULL;
  ChannelDetails* _public_channel = NULL;
  uint32_t _expected_ack_crc = 0;

  // ContactVisitor — we don't surface contacts in UI yet.
  void onContactVisit(const ContactInfo& contact) override {}

  // Pure virtual stubs (we don't use these features yet).
  void onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) override {}
  ContactInfo* processAck(const uint8_t *data) override {
    if (memcmp(data, &_expected_ack_crc, 4) == 0) {
      _expected_ack_crc = 0;
    }
    return NULL;
  }
  void onContactPathUpdated(const ContactInfo& contact) override {}
  void onMessageRecv(const ContactInfo& contact, mesh::Packet* pkt, uint32_t sender_timestamp, const char *text) override {
    Serial.printf("[mesh] direct msg from %s: %s\n", contact.name, text);
    moki_lora_push_msg_external(contact.name, text, 0);
  }
  void onCommandDataRecv(const ContactInfo& contact, mesh::Packet* pkt, uint32_t sender_timestamp, const char *text) override {}
  void onSignedMessageRecv(const ContactInfo& contact, mesh::Packet* pkt, uint32_t sender_timestamp, const uint8_t *sender_prefix, const char *text) override {}
  uint8_t onContactRequest(const ContactInfo& contact, uint32_t sender_timestamp, const uint8_t* data, uint8_t len, uint8_t* reply) override { return 0; }
  void onContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len) override {}

  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override {
    return SEND_TIMEOUT_BASE_MILLIS + (uint32_t)(FLOOD_SEND_TIMEOUT_FACTOR * pkt_airtime_millis);
  }
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override {
    uint8_t hops = path_len & 63;
    return SEND_TIMEOUT_BASE_MILLIS +
      (uint32_t)((pkt_airtime_millis * DIRECT_SEND_PERHOP_FACTOR + DIRECT_SEND_PERHOP_EXTRA_MILLIS) * (hops + 1));
  }
  void onSendTimeout() override {
    Serial.println(F("[mesh] tx timeout (no ACK)"));
  }

  // The channel-message hook — what we actually care about right now.
  // MeshCore wire format: "<sender>: <body>" — we split here so the chat-
  // detail bubbles show sender + text separately, like a real chat client.
  void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t timestamp, const char *text) override {
    Serial.printf("[mesh] channel msg (%s, %d hops): %s\n",
                  pkt->isRouteDirect() ? "direct" : "flood",
                  pkt->path_len, text);
    const char *colon = strstr(text, ": ");
    if (colon && (colon - text) < 23) {
      // Split sender (up to 23 chars) and body.
      char sender[24];
      size_t name_len = colon - text;
      if (name_len >= sizeof(sender)) name_len = sizeof(sender) - 1;
      memcpy(sender, text, name_len);
      sender[name_len] = 0;
      moki_lora_push_msg_external(sender, colon + 2, 0);
    } else {
      moki_lora_push_msg_external("mesh", text, 0);
    }
  }

public:
  // Pool size 16 (the simple_secure_chat default) drained quickly indoors
  // because the rx_queue fills with delayed-flood packets from the busy
  // 869.618 MHz Heidelberg mesh. 64 gives comfortable breathing room.
  MyMesh(mesh::Radio& radio, mesh::RNG& rng, mesh::RTCClock& rtc, SimpleMeshTables& tables)
    : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc, *new StaticPoolPacketManager(64), tables)
  {}

  void beginWithIdentity(mesh::RNG &seed_rng) {
    BaseChatMesh::begin();

    // Build Identity from a deterministic seed-RNG (caller fills it from
    // g_identity_secret) so the same physical Moki always has the same
    // pubkey across reboots.
    self_id = mesh::LocalIdentity(&seed_rng);

    // Channel from settings (user-configurable name + Base64 PSK).
    const char *name = moki_settings_get_channel_name();
    const char *psk  = moki_settings_get_channel_psk();
    _moki_channel = addChannel(name, psk);

    Serial.printf("[mesh] identity set, channel '%s' (%s)\n",
                  name, _moki_channel ? "ok" : "FAILED");
  }

  // Returns true if message was queued for transmission.
  bool sendToMokiChannel(const char *sender_name, const char *text) {
    if (!_moki_channel) return false;
    uint32_t ts = getRTCClock()->getCurrentTimeUnique();
    bool ok = sendGroupMessage(ts, _moki_channel->channel, sender_name, text, strlen(text));
    Serial.printf("[mesh] tx '%s': '%s' (%s)\n",
                  sender_name, text, ok ? "queued" : "FAILED");
    return ok;
  }
};

// ── Globals + C-API for main.cpp ───────────────────────────────────────────

static StdRNG          g_mesh_rng;
static SimpleRTCClock  g_mesh_rtc;
static SimpleMeshTables g_mesh_tables;
static MyMesh*         g_mesh = NULL;

extern "C" {

void moki_mesh_init(void) {
  if (g_mesh) return;
  // BaseChatMesh's begin() expects radio.begin() to have been done already.
  // radio_init() in target.cpp does it. main.cpp calls that.
  g_mesh = new MyMesh(radio_driver, g_mesh_rng, g_mesh_rtc, g_mesh_tables);
  // Deterministic identity from g_identity_secret — same pubkey across reboots.
  SeedRNG seed_rng(g_identity_secret);
  g_mesh->beginWithIdentity(seed_rng);
}

void moki_mesh_loop(void) {
  if (g_mesh) g_mesh->loop();
}

bool moki_mesh_send(const char *sender_name, const char *text) {
  if (!g_mesh) return false;
  return g_mesh->sendToMokiChannel(sender_name, text);
}

}
