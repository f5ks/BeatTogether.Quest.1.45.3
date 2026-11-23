#include "config.hpp"
#include "hooking.hpp"

#include "GlobalNamespace/BeatSaberPlayerIdentityPacketData.hpp"
#include "GlobalNamespace/ByteArrayNetSerializable.hpp"
#include "GlobalNamespace/PlayerConnectedPacket.hpp"
#include "GlobalNamespace/PlayerIdentityPacket_1.hpp"
#include "LiteNetLib/Utils/NetDataReader.hpp"
#include "LiteNetLib/Utils/NetDataWriter.hpp"

namespace {
    using namespace GlobalNamespace;
    using namespace LiteNetLib::Utils;
    using IdentityPacket = PlayerIdentityPacket_1<BeatSaberPlayerIdentityPacketData>;
}

MAKE_AUTO_HOOK_MATCH(PlayerConnectedPacket_Deserialize, &PlayerConnectedPacket::Deserialize,
    void, PlayerConnectedPacket* self, NetDataReader* reader) {
    if (!UsesBeatTogetherLegacyServer()) {
        PlayerConnectedPacket_Deserialize(self, reader);
        return;
    }

    self->remoteConnectionId = reader->GetByte();
    self->userId = reader->GetString();
    self->userName = reader->GetString();
    self->isConnectionOwner = reader->GetBool();
    self->compatibilityVersion = StringW("");
}

MAKE_AUTO_HOOK_FIND_VERBOSE(PlayerIdentityPacket_Serialize,
    (i2c::find_method(i2c::class_of<IdentityPacket*>(), {"Serialize", {}, {i2c::type_of<NetDataWriter*>()}})),
    void, IdentityPacket* self, NetDataWriter* writer, const MethodInfo* method) {
    if (!UsesBeatTogetherLegacyServer()) {
        PlayerIdentityPacket_Serialize(self, writer, method);
        return;
    }

    self->playerState.Serialize(writer);
    // Legacy identity puts the avatar before the random and public key and has no active-hand byte.
    self->gameSpecificData.playerAvatar.Serialize(writer);
    self->random->Serialize(writer);
    self->publicEncryptionKey->Serialize(writer);
}

MAKE_AUTO_HOOK_FIND_VERBOSE(PlayerIdentityPacket_Deserialize,
    (i2c::find_method(i2c::class_of<IdentityPacket*>(), {"Deserialize", {}, {i2c::type_of<NetDataReader*>()}})),
    void, IdentityPacket* self, NetDataReader* reader, const MethodInfo* method) {
    if (!UsesBeatTogetherLegacyServer()) {
        PlayerIdentityPacket_Deserialize(self, reader, method);
        return;
    }

    self->playerState = PlayerStateHash::Deserialize(reader);
    self->gameSpecificData = BeatSaberPlayerIdentityPacketData(
        MultiplayerAvatarsData::Deserialize(reader), MultiplayerActiveHand::None);
    self->random->Deserialize(reader);
    self->publicEncryptionKey->Deserialize(reader);
}
