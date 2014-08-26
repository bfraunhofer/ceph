// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_MMDSBEACON_H
#define CEPH_MMDSBEACON_H

#include "messages/PaxosServiceMessage.h"

#include "include/types.h"

#include "mds/MDSMap.h"

#include <uuid/uuid.h>


/**
 * Health metrics send by the MDS to the mon, so that the mon can generate
 * user friendly warnings about undesirable states.
 */
struct MDSHealth
{
  // In order that we can indicate to the user
  // if the MDS is too far behind trimming
  uint32_t log_segment_count;
  uint32_t log_max_segments;

  void encode(bufferlist& bl) const {
    ::encode(log_segment_count, bl);
    ::encode(log_max_segments, bl);
  }
  
  void decode(bufferlist::iterator& bl) {
    ::decode(log_segment_count, bl);
    ::decode(log_max_segments, bl);
  }

  MDSHealth() : log_segment_count(0), log_max_segments(0) {}
};
WRITE_CLASS_ENCODER(MDSHealth)

class MMDSBeacon : public PaxosServiceMessage {

  static const int HEAD_VERSION = 3;

  uuid_d fsid;
  uint64_t global_id;
  string name;

  MDSMap::DaemonState state;
  version_t seq;
  __s32 standby_for_rank;
  string standby_for_name;

  CompatSet compat;

  MDSHealth health;

 public:
  MMDSBeacon() : PaxosServiceMessage(MSG_MDS_BEACON, 0, HEAD_VERSION) { }
  MMDSBeacon(const uuid_d &f, uint64_t g, string& n, epoch_t les, MDSMap::DaemonState st, version_t se) : 
    PaxosServiceMessage(MSG_MDS_BEACON, les, HEAD_VERSION), 
    fsid(f), global_id(g), name(n), state(st), seq(se),
    standby_for_rank(-1) {
  }
private:
  ~MMDSBeacon() {}

public:
  uuid_d& get_fsid() { return fsid; }
  uint64_t get_global_id() { return global_id; }
  string& get_name() { return name; }
  epoch_t get_last_epoch_seen() { return version; }
  MDSMap::DaemonState get_state() { return state; }
  version_t get_seq() { return seq; }
  const char *get_type_name() const { return "mdsbeacon"; }
  int get_standby_for_rank() { return standby_for_rank; }
  const string& get_standby_for_name() { return standby_for_name; }

  CompatSet const& get_compat() const { return compat; }
  void set_compat(const CompatSet& c) { compat = c; }

  MDSHealth const& get_health() const { return health; }
  void set_health(const MDSHealth &h) { health = h; }

  void set_standby_for_rank(int r) { standby_for_rank = r; }
  void set_standby_for_name(string& n) { standby_for_name = n; }
  void set_standby_for_name(const char* c) { standby_for_name.assign(c); }

  void print(ostream& out) const {
    out << "mdsbeacon(" << global_id << "/" << name << " " << ceph_mds_state_name(state) 
	<< " seq " << seq << " v" << version << ")";
  }

  void encode_payload(uint64_t features) {
    paxos_encode();
    ::encode(fsid, payload);
    ::encode(global_id, payload);
    ::encode((__u32)state, payload);
    ::encode(seq, payload);
    ::encode(name, payload);
    ::encode(standby_for_rank, payload);
    ::encode(standby_for_name, payload);
    ::encode(compat, payload);
    ::encode(health, payload);
  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    paxos_decode(p);
    ::decode(fsid, p);
    ::decode(global_id, p);
    ::decode((__u32&)state, p);
    ::decode(seq, p);
    ::decode(name, p);
    ::decode(standby_for_rank, p);
    ::decode(standby_for_name, p);
    if (header.version >= 2)
      ::decode(compat, p);
    if (header.version >= 3) {
      ::decode(health, payload);
    }
  }
};

#endif
