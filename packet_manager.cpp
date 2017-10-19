#include <iostream>

#include <ctime>
#include <process.h>

#include "packet_manager.h"
#include "log.h"


namespace taosocks {
ClientPacketManager::ClientPacketManager()
    : _seq(0)
    , _worker(-1)
{
    LogLog("创建客户端包管理器");

    ::UuidCreate(&_guid);

    _worker.OnConnect([this](ClientSocket*, bool connected) {
        return _OnConnect(connected);
    });

    _worker.OnRead([this](ClientSocket*, unsigned char* data, size_t size) {
        return _OnRead(data, size);
    });

    _worker.OnWrite([this](ClientSocket*, size_t size) {
        return _OnWrite();
    });

    _worker.OnClose([this](ClientSocket* c, CloseReason reason) {
        return _OnClose(reason);
    });
}

void ClientPacketManager::Read()
{
    _worker.Read();
}

void ClientPacketManager::Send(BasePacket* pkt)
{
    assert(pkt != nullptr);
    std::memcpy(&pkt->__guid, &_guid, sizeof(GUID));
    pkt->__seq = ++_seq;
    _packet = pkt;
    if(!_worker.IsClosed()) {
        _worker.Write((char*)pkt, pkt->__size);
        LogLog("发送数据包(%d)：sid=%d, cid=%d, seq=%d, cmd=%d, size=%d",
            _worker.GetId(),
            pkt->__sid, pkt->__cid,
            pkt->__seq, pkt->__cmd, pkt->__size);
    }
    else {
        _Connect();
    }
}

void ClientPacketManager::_OnRead(unsigned char* data, size_t size)
{
    auto& recv_data = _recv_data;
    recv_data.append(data, size);

    auto more = true;

    for(BasePacket* bpkt; (bpkt = recv_data.try_cast<BasePacket>()) != nullptr && (int)recv_data.size() >= bpkt->__size;) {
        LogLog("收到数据包(%d) sid=%d, cid=%d, seq=%d, cmd=%d, size=%d",
            _worker.GetId(), bpkt->__sid, bpkt->__cid, bpkt->__seq, bpkt->__cmd, bpkt->__size);
        auto pkt = new (new char[bpkt->__size]) BasePacket;
        recv_data.get(pkt, bpkt->__size);
        assert(OnPacketRead);
        more = OnPacketRead(pkt);
    }

    if(more) {
        _worker.Read();
    }
}

void ClientPacketManager::_OnWrite()
{
    assert(OnPacketSent);
    return OnPacketSent();
}

void ClientPacketManager::_OnConnect(bool connected)
{
    if(!connected) {
        assert(OnError);
        OnError();
        return;
    }

    _worker.Read();

    if(_packet != nullptr) {
        _worker.Write(_packet, _packet->__size);
    }
    else {
        assert(OnPacketSent);
        OnPacketSent();
    }
}

void ClientPacketManager::_OnClose(CloseReason reason)
{
    if(reason == CloseReason::Passively || reason == CloseReason::Reset) {
        _worker.Close();
        _Connect();
    }
}

void ClientPacketManager::_Connect()
{
    if(_worker.IsClosed()) {
        in_addr addr;
        addr.S_un.S_addr = ::inet_addr("127.0.0.1");
        _worker.Connect(addr, 8081);
    }
}

//////////////////////////////////////////////////////////////////////////

ServerPacketManager::ServerPacketManager()
    : _seq(0)
{
}

void ServerPacketManager::StartPassive()
{
    LogLog("被动打开");
}

void ServerPacketManager::Send(BasePacket* pkt)
{
    assert(pkt != nullptr);
    pkt->__seq = ++_seq;
    _packets.push_back(pkt);
    Schedule();
}


void ServerPacketManager::AddClient(ClientSocket* client)
{
    client->Read();

    client->OnRead([this](ClientSocket* client, unsigned char* data, size_t size) {
        return OnRead(client, data, size);
    });

    client->OnWrite([](ClientSocket* client, size_t size) {

    });

    // 可能是掉线、可能是正常关闭
    // 掉线不清理网站连接，正常关闭要清理
    client->OnClose([this](ClientSocket*, CloseReason reason) {
        if(reason == CloseReason::Actively) {
            LogLog("主动关闭连接，网站先断开");
            // 放在网站断开连接那儿去处理
        }
        else if(reason == CloseReason::Passively) {
            LogLog("被动关闭连接");

        }
        else if(reason == CloseReason::Reset) {
            LogLog("连接被重置");
        }
        else {
            LogWrn("Bad Reason");
        }
    });
}

void ServerPacketManager::RemoveClient(ClientSocket* client)
{

}

void ServerPacketManager::Schedule()
{
    if(_packets.empty()) {
        return;
    }

    assert(!_clients.empty());

    auto pkt = _packets.front();
    _packets.pop_front();

    auto range = _clients.equal_range(pkt->__guid);
    assert(range.first != range.second);

    auto index = std::rand() % _clients.count(pkt->__guid);
    auto client = _clients[pkt->__guid][index];

    client->Write((char*)pkt, pkt->__size);
    LogLog("发送数据包(%d)：sid=%d, cid=%d, seq=%d, cmd=%d, size=%d", client->GetId(), pkt->__sid, pkt->__cid, pkt->__seq, pkt->__cmd, pkt->__size);
}

void ServerPacketManager::OnRead(ClientSocket* client, unsigned char* data, size_t size)
{
    auto& recv_data = _recv_data[client];
    recv_data.append(data, size);

    for(BasePacket* bpkt; (bpkt = recv_data.try_cast<BasePacket>()) != nullptr && (int)recv_data.size() >= bpkt->__size;) {
        if(bpkt->__cmd == PacketCommand::Connect) {
            _clients[bpkt->__guid].push_back(client);
        }

        LogLog("收到数据包(%d)：sid=%d, cid=%d, seq=%d, cmd=%d, size=%d", client->GetId(), bpkt->__sid, bpkt->__cid, bpkt->__seq, bpkt->__cmd, bpkt->__size);

        auto pkt = new (new char[bpkt->__size]) BasePacket;
        recv_data.get(pkt, bpkt->__size);
        auto handler = _handlers.find(pkt->__sid);
        if(handler == _handlers.cend()) {
            LogWrn("包没有处理器，丢弃。");
        }
        else {
            handler->second->OnPacket(pkt);
        }
    }

    client->Read();
}

}