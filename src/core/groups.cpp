// LunaShare - group conversations: several people (accounts) in one chat, and in one call.
//
// A group is a list of member accounts plus a version. Whoever changes it (adds people, renames it, leaves) sends the
// new list to every device of every member (GroupInfo), together with the signed device lists of the members' accounts
// that the sender knows, so members who have never met can reach each other (and verify who they are talking to).
// Messages are sent to every member device separately, tagged with the group id ("gid"); the receivers file them under
// "#" + id. Nothing about a group is kept on the server.
#include <algorithm>
#include <set>

#include "engine.h"
#include "platform.h"

namespace ls {

static constexpr size_t kMaxGroupMembers = 32;

json Engine::GroupJson(const Group& g) {
    json ms = json::array();
    for (auto& m : g.members) ms.push_back({{"acc", m.acc}, {"name", m.name}});
    return {{"gid", g.id}, {"name", g.name}, {"creator", g.creator}, {"ver", g.ver}, {"members", ms}};
}

bool Engine::GroupOnlyHidden(const std::string& device) {
    if (!settings.groupsViaServer) return false;
    auto d = devices.find(device);
    return d != devices.end() && d->second.viaGroup && !d->second.trusted;
}

json Engine::KeyField(const std::string& key, json j) {
    if (IsGroupKey(key)) j["gid"] = key.substr(1);
    return j;
}

const Group* Engine::GroupOf(const std::string& key) {
    if (!IsGroupKey(key)) return nullptr;
    auto g = groups.find(key.substr(1));
    return g == groups.end() ? nullptr : &g->second;
}

Group& Engine::EnsureGroup(const std::string& gid, const std::string& senderAcc) {
    Group& g = groups[gid];
    if (g.id.empty()) {
        g.id = gid;  // version 0: filled in when its GroupInfo arrives
        if (!senderAcc.empty()) g.members.push_back({senderAcc, PeerName("@" + senderAcc)});
        if (!MyAccount().empty()) g.members.push_back({MyAccount(), settings.displayName});
        MarkDirty();
    }
    return g;
}

// The conversation an incoming chat frame belongs to: the sender's own (person or device), or a group the sender is in.
// Empty: not accepted (a group we left, or the sender isn't a member).
std::string Engine::FrameKey(const std::shared_ptr<Connection>& c, const json& j) {
    std::string gid = j.value("gid", "");
    if (gid.empty()) return ChatKey(c->peerId);
    if (gid.size() > 40) return "";
    auto dv = devices.find(c->peerId);
    if (dv == devices.end() || dv->second.accountPub.empty()) return "";
    const std::string& acc = dv->second.accountPub;
    auto g = groups.find(gid);
    if (g == groups.end()) {
        if (acc == MyAccount()) return "";
        EnsureGroup(gid, acc);  // a message that came before the group's description (e.g. from the mailbox)
        return "#" + gid;
    }
    if (g->second.left) return "";
    if (acc != MyAccount() && !g->second.Has(acc)) return "";
    return "#" + gid;
}

void Engine::SendGroupInfo(const Group& g, const std::vector<std::string>& alsoTo) {
    json j = GroupJson(g);
    json small = j;
    // the members' signed device lists, so the others can find and verify their devices
    json accts = json::array();
    for (auto& m : g.members) {
        if (m.acc == MyAccount()) {
            if (!settings.accountSig.empty()) accts.push_back(AccountJson());
            continue;
        }
        auto a = accounts.find(m.acc);
        if (a == accounts.end() || a->second.sig.empty()) continue;
        json ds = json::array();
        for (auto& d : a->second.devices) ds.push_back({{"id", d.id}, {"pub", d.pub}});
        accts.push_back({{"pub", m.acc}, {"ver", a->second.ver}, {"devices", ds}, {"removed", a->second.removed}, {"sig", a->second.sig}});
    }
    j["accts"] = accts;
    std::set<std::string> to;
    for (auto& m : g.members)
        if (m.acc != MyAccount())
            for (auto& d : DevicesOf("@" + m.acc)) to.insert(d);
    for (auto& acc : alsoTo)
        for (auto& d : DevicesOf("@" + acc)) to.insert(d);
    for (auto& d : MyOtherDevices()) to.insert(d);
    for (auto& d : to)
        if (!QueueToPeer(d, Msg::GroupInfo, j) && !MailTo(d, Msg::GroupInfo, j)) MailTo(d, Msg::GroupInfo, small);
}

void Engine::HandleGroupInfo(const std::shared_ptr<Connection>& c, const json& j) {
    auto dv = devices.find(c->peerId);
    if (dv == devices.end() || dv->second.accountPub.empty() || dv->second.blocked) return;
    const std::string sender = dv->second.accountPub;
    const bool fromMine = sender == MyAccount();
    std::string gid = j.value("gid", "");
    if (gid.empty() || gid.size() > 40) return;
    Group n;
    n.id = gid;
    n.name = j.value("name", "");
    if (n.name.size() > 100) n.name = n.name.substr(0, 100);
    n.creator = j.value("creator", "");
    n.ver = j.value("ver", (int64_t)0);
    std::set<std::string> seen;
    for (auto& m : j.value("members", json::array())) {
        if (!m.is_object()) continue;
        std::string acc = m.value("acc", ""), name = m.value("name", "");
        if (acc.size() != 64 || !seen.insert(acc).second) continue;
        if (name.size() > 100) name = name.substr(0, 100);
        n.members.push_back({acc, name});
    }
    if (n.members.size() > kMaxGroupMembers || n.ver <= 0) return;
    auto it = groups.find(gid);
    bool existed = it != groups.end() && it->second.ver > 0;
    Group old = it != groups.end() ? it->second : Group{};
    if (existed && n.ver <= old.ver) return;
    if (!fromMine) {
        // who may change what: anyone in the group may add people and rename it; people may take only themselves
        // off, except the creator, who may remove anyone
        bool senderWasIn = existed ? old.Has(sender) : n.Has(sender);
        if (!senderWasIn) return;
        if (existed && !old.creator.empty() && n.creator != old.creator) return;
        if (existed)
            for (auto& m : old.members)
                if (!n.Has(m.acc) && m.acc != sender && sender != old.creator) return;
        if (!existed && !n.Has(MyAccount())) return;  // a group I'm not in
    }
    // the members' devices
    for (auto& a : j.value("accts", json::array()))
        if (a.is_object() && n.Has(a.value("pub", ""))) ApplyAccountList(a, true);
    for (auto& m : n.members)
        for (auto& d : DevicesOf("@" + m.acc)) {
            KnownDevice& kd = devices[d];
            if (kd.viaGroup && !m.name.empty() && (kd.name.empty() || kd.name == m.acc.substr(0, 8))) kd.name = m.name;
        }
    // what changed, for the conversation
    std::string key = "#" + gid;
    auto nameOf = [&](const std::string& acc) {
        if (acc == MyAccount()) return std::string("You");
        for (auto& m : n.members)
            if (m.acc == acc && !m.name.empty()) return m.name;
        for (auto& m : old.members)
            if (m.acc == acc && !m.name.empty()) return m.name;
        return PeerName("@" + acc);
    };
    bool meIn = n.Has(MyAccount());
    if (!fromMine) {
        if (!existed || old.left) {
            if (meIn) AddSystemMessage(key, nameOf(sender) + " added you to the group.");
        } else {
            std::vector<std::string> added, gone;
            for (auto& m : n.members)
                if (!old.Has(m.acc)) added.push_back(nameOf(m.acc));
            for (auto& m : old.members)
                if (!n.Has(m.acc)) {
                    if (m.acc == sender) AddSystemMessage(key, nameOf(m.acc) + " left the group.");
                    else if (m.acc == MyAccount()) AddSystemMessage(key, nameOf(sender) + " removed you from the group.");
                    else gone.push_back(nameOf(m.acc));
                }
            auto list = [](const std::vector<std::string>& v) {
                std::string s;
                for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + v[i];
                return s;
            };
            if (!added.empty()) AddSystemMessage(key, nameOf(sender) + " added " + list(added) + ".");
            if (!gone.empty()) AddSystemMessage(key, nameOf(sender) + " removed " + list(gone) + ".");
            if (existed && n.name != old.name) AddSystemMessage(key, nameOf(sender) + " named the group \"" + n.name + "\".");
        }
    }
    n.left = !meIn;
    groups[gid] = n;
    chats[key].peer = key;
    if (!fromMine && (!existed || old.left) && meIn) Emit({EventType::ChatReceived, key, nameOf(sender) + " added you to the group."});
    MarkDirty();
}

// ---- actions

std::string Engine::CreateGroup(const std::string& name, const std::vector<std::string>& people) {
    auto lk = Lock();
    if (MyAccount().empty()) return "";
    Group g;
    g.id = RandomHex(12);
    g.name = name.substr(0, 100);
    g.creator = MyAccount();
    g.ver = NowMs();
    g.members.push_back({MyAccount(), settings.displayName});
    for (auto& p : people) {
        if (!IsContactKey(p) || p.size() != 65 || g.Has(p.substr(1))) continue;
        if (g.members.size() >= kMaxGroupMembers) break;
        g.members.push_back({p.substr(1), PeerName(p)});
    }
    groups[g.id] = g;
    std::string key = "#" + g.id;
    chats[key].peer = key;
    AddSystemMessage(key, "You created the group.");
    SendGroupInfo(g);
    MarkDirty();
    return key;
}

void Engine::AddToGroup(const std::string& key, const std::vector<std::string>& people) {
    auto lk = Lock();
    auto it = groups.find(IsGroupKey(key) ? key.substr(1) : "");
    if (it == groups.end() || it->second.left) return;
    Group& g = it->second;
    std::vector<std::string> names;
    for (auto& p : people) {
        if (!IsContactKey(p) || p.size() != 65 || g.Has(p.substr(1))) continue;
        if (g.members.size() >= kMaxGroupMembers) break;
        g.members.push_back({p.substr(1), PeerName(p)});
        names.push_back(PeerName(p));
    }
    if (names.empty()) return;
    g.ver = std::max(NowMs(), g.ver + 1);
    std::string s;
    for (size_t i = 0; i < names.size(); i++) s += (i ? ", " : "") + names[i];
    AddSystemMessage(key, "You added " + s + ".");
    SendGroupInfo(g);
    MarkDirty();
}

void Engine::RemoveFromGroup(const std::string& key, const std::string& acc) {
    auto lk = Lock();
    auto it = groups.find(IsGroupKey(key) ? key.substr(1) : "");
    if (it == groups.end() || it->second.left) return;
    Group& g = it->second;
    if (g.creator != MyAccount() || acc == MyAccount() || !g.Has(acc)) return;
    std::string name = PeerName("@" + acc);
    for (auto& m : g.members)
        if (m.acc == acc && !m.name.empty()) name = m.name;
    g.members.erase(std::remove_if(g.members.begin(), g.members.end(), [&](const GroupMember& m) { return m.acc == acc; }), g.members.end());
    g.ver = std::max(NowMs(), g.ver + 1);
    AddSystemMessage(key, "You removed " + name + ".");
    SendGroupInfo(g, {acc});  // they learn it too (and stop sending)
    MarkDirty();
}

void Engine::LeaveGroup(const std::string& key) {
    auto lk = Lock();
    auto it = groups.find(IsGroupKey(key) ? key.substr(1) : "");
    if (it == groups.end() || it->second.left) return;
    Group& g = it->second;
    g.members.erase(std::remove_if(g.members.begin(), g.members.end(), [&](const GroupMember& m) { return m.acc == MyAccount(); }), g.members.end());
    g.ver = std::max(NowMs(), g.ver + 1);
    SendGroupInfo(g);
    g.left = true;
    AddSystemMessage(key, "You left the group.");
    MarkDirty();
}

void Engine::RenameGroup(const std::string& key, const std::string& name) {
    auto lk = Lock();
    auto it = groups.find(IsGroupKey(key) ? key.substr(1) : "");
    if (it == groups.end() || it->second.left || name.empty() || name == it->second.name) return;
    Group& g = it->second;
    g.name = name.substr(0, 100);
    g.ver = std::max(NowMs(), g.ver + 1);
    AddSystemMessage(key, "You named the group \"" + g.name + "\".");
    SendGroupInfo(g);
    MarkDirty();
}

}  // namespace ls
