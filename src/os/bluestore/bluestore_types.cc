// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "bluestore_types.h"
#include "common/Formatter.h"
#include "common/Checksummer.h"
#include "include/stringify.h"

void ExtentList::add_extents(int64_t start, int64_t count) {
  AllocExtent *last_extent = NULL;
  bool can_merge = false;

  if (!m_extents->empty()) {
    last_extent = &(m_extents->back());
    uint64_t last_offset = last_extent->end() / m_block_size;
    uint32_t last_length = last_extent->length / m_block_size;
    if ((last_offset == (uint64_t) start) &&
        (!m_max_blocks || (last_length + count) <= m_max_blocks)) {
      can_merge = true;
    }
  }

  if (can_merge) {
    last_extent->length += (count * m_block_size);
  } else {
    m_extents->emplace_back(AllocExtent(start * m_block_size,
					count * m_block_size));
  }
}

// bluestore_bdev_label_t

void bluestore_bdev_label_t::encode(bufferlist& bl) const
{
  // be slightly friendly to someone who looks at the device
  bl.append("bluestore block device\n");
  bl.append(stringify(osd_uuid));
  bl.append("\n");
  ENCODE_START(1, 1, bl);
  ::encode(osd_uuid, bl);
  ::encode(size, bl);
  ::encode(btime, bl);
  ::encode(description, bl);
  ENCODE_FINISH(bl);
}

void bluestore_bdev_label_t::decode(bufferlist::iterator& p)
{
  p.advance(60); // see above
  DECODE_START(1, p);
  ::decode(osd_uuid, p);
  ::decode(size, p);
  ::decode(btime, p);
  ::decode(description, p);
  DECODE_FINISH(p);
}

void bluestore_bdev_label_t::dump(Formatter *f) const
{
  f->dump_stream("osd_uuid") << osd_uuid;
  f->dump_unsigned("size", size);
  f->dump_stream("btime") << btime;
  f->dump_string("description", description);
}

void bluestore_bdev_label_t::generate_test_instances(
  list<bluestore_bdev_label_t*>& o)
{
  o.push_back(new bluestore_bdev_label_t);
  o.push_back(new bluestore_bdev_label_t);
  o.back()->size = 123;
  o.back()->btime = utime_t(4, 5);
  o.back()->description = "fakey";
}

ostream& operator<<(ostream& out, const bluestore_bdev_label_t& l)
{
  return out << "bdev(osd_uuid " << l.osd_uuid
	     << " size 0x" << std::hex << l.size << std::dec
	     << " btime " << l.btime
	     << " desc " << l.description << ")";
}

// cnode_t

void bluestore_cnode_t::dump(Formatter *f) const
{
  f->dump_unsigned("bits", bits);
}

void bluestore_cnode_t::generate_test_instances(list<bluestore_cnode_t*>& o)
{
  o.push_back(new bluestore_cnode_t());
  o.push_back(new bluestore_cnode_t(0));
  o.push_back(new bluestore_cnode_t(123));
}

// bluestore_extent_ref_map_t

void bluestore_extent_ref_map_t::_check() const
{
  uint64_t pos = 0;
  unsigned refs = 0;
  for (const auto &p : ref_map) {
    if (p.first < pos)
      assert(0 == "overlap");
    if (p.first == pos && p.second.refs == refs)
      assert(0 == "unmerged");
    pos = p.first + p.second.length;
    refs = p.second.refs;
  }
}

void bluestore_extent_ref_map_t::_maybe_merge_left(
  map<uint64_t,record_t>::iterator& p)
{
  if (p == ref_map.begin())
    return;
  auto q = p;
  --q;
  if (q->second.refs == p->second.refs &&
      q->first + q->second.length == p->first) {
    q->second.length += p->second.length;
    ref_map.erase(p);
    p = q;
  }
}

void bluestore_extent_ref_map_t::get(uint64_t offset, uint32_t length)
{
  auto p = ref_map.lower_bound(offset);
  if (p != ref_map.begin()) {
    --p;
    if (p->first + p->second.length <= offset) {
      ++p;
    }
  }
  while (length > 0) {
    if (p == ref_map.end()) {
      // nothing after offset; add the whole thing.
      p = ref_map.insert(
	map<uint64_t,record_t>::value_type(offset, record_t(length, 1))).first;
      break;
    }
    if (p->first > offset) {
      // gap
      uint64_t newlen = MIN(p->first - offset, length);
      p = ref_map.insert(
	map<uint64_t,record_t>::value_type(offset,
					   record_t(newlen, 1))).first;
      offset += newlen;
      length -= newlen;
      _maybe_merge_left(p);
      ++p;
      continue;
    }
    if (p->first < offset) {
      // split off the portion before offset
      assert(p->first + p->second.length > offset);
      uint64_t left = p->first + p->second.length - offset;
      p->second.length = offset - p->first;
      p = ref_map.insert(map<uint64_t,record_t>::value_type(
			   offset, record_t(left, p->second.refs))).first;
      // continue below
    }
    assert(p->first == offset);
    if (length < p->second.length) {
      ref_map.insert(make_pair(offset + length,
			       record_t(p->second.length - length,
					p->second.refs)));
      p->second.length = length;
      ++p->second.refs;
      break;
    }
    ++p->second.refs;
    offset += p->second.length;
    length -= p->second.length;
    _maybe_merge_left(p);
    ++p;
  }
  if (p != ref_map.end())
    _maybe_merge_left(p);
  //_check();
}

void bluestore_extent_ref_map_t::put(
  uint64_t offset, uint32_t length,
  PExtentVector *release)
{
  //NB: existing entries in 'release' container must be preserved!

  auto p = ref_map.lower_bound(offset);
  if (p == ref_map.end() || p->first > offset) {
    if (p == ref_map.begin()) {
      assert(0 == "put on missing extent (nothing before)");
    }
    --p;
    if (p->first + p->second.length <= offset) {
      assert(0 == "put on missing extent (gap)");
    }
  }
  if (p->first < offset) {
    uint64_t left = p->first + p->second.length - offset;
    p->second.length = offset - p->first;
    p = ref_map.insert(map<uint64_t,record_t>::value_type(
			 offset, record_t(left, p->second.refs))).first;
  }
  while (length > 0) {
    assert(p->first == offset);
    if (length < p->second.length) {
      ref_map.insert(make_pair(offset + length,
			       record_t(p->second.length - length,
					p->second.refs)));
      if (p->second.refs > 1) {
	p->second.length = length;
	--p->second.refs;
	_maybe_merge_left(p);
      } else {
	if (release)
	  release->push_back(bluestore_pextent_t(p->first, length));
	ref_map.erase(p);
      }
      return;
    }
    offset += p->second.length;
    length -= p->second.length;
    if (p->second.refs > 1) {
      --p->second.refs;
      _maybe_merge_left(p);
      ++p;
    } else {
      if (release)
	release->push_back(bluestore_pextent_t(p->first, p->second.length));
      ref_map.erase(p++);
    }
  }
  if (p != ref_map.end())
    _maybe_merge_left(p);
  //_check();
}

bool bluestore_extent_ref_map_t::contains(uint64_t offset, uint32_t length) const
{
  auto p = ref_map.lower_bound(offset);
  if (p == ref_map.end() || p->first > offset) {
    if (p == ref_map.begin()) {
      return false; // nothing before
    }
    --p;
    if (p->first + p->second.length <= offset) {
      return false; // gap
    }
  }
  while (length > 0) {
    if (p == ref_map.end())
      return false;
    if (p->first > offset)
      return false;
    if (p->first + p->second.length >= offset + length)
      return true;
    uint64_t overlap = p->first + p->second.length - offset;
    offset += overlap;
    length -= overlap;
    ++p;
  }
  return true;
}

bool bluestore_extent_ref_map_t::intersects(
  uint64_t offset,
  uint32_t length) const
{
  auto p = ref_map.lower_bound(offset);
  if (p != ref_map.begin()) {
    --p;
    if (p->first + p->second.length <= offset) {
      ++p;
    }
  }
  if (p == ref_map.end())
    return false;
  if (p->first >= offset + length)
    return false;
  return true;  // intersects p!
}

void bluestore_extent_ref_map_t::dump(Formatter *f) const
{
  f->open_array_section("ref_map");
  for (auto& p : ref_map) {
    f->open_object_section("ref");
    f->dump_unsigned("offset", p.first);
    f->dump_unsigned("length", p.second.length);
    f->dump_unsigned("refs", p.second.refs);
    f->close_section();
  }
  f->close_section();
}

void bluestore_extent_ref_map_t::generate_test_instances(
  list<bluestore_extent_ref_map_t*>& o)
{
  o.push_back(new bluestore_extent_ref_map_t);
  o.push_back(new bluestore_extent_ref_map_t);
  o.back()->get(10, 10);
  o.back()->get(18, 22);
  o.back()->get(20, 20);
  o.back()->get(10, 25);
  o.back()->get(15, 20);
}

ostream& operator<<(ostream& out, const bluestore_extent_ref_map_t& m)
{
  out << "ref_map(";
  for (auto p = m.ref_map.begin(); p != m.ref_map.end(); ++p) {
    if (p != m.ref_map.begin())
      out << ",";
    out << std::hex << "0x" << p->first << "~" << p->second.length << std::dec
	<< "=" << p->second.refs;
  }
  out << ")";
  return out;
}

// bluestore_blob_use_tracker_t

void bluestore_blob_use_tracker_t::allocate()
{
  assert(num_au != 0);
  bytes_per_au = new uint32_t[num_au];
  for (uint32_t i = 0; i < num_au; ++i) {
    bytes_per_au[i] = 0;
  }
}

void bluestore_blob_use_tracker_t::init(
  uint32_t full_length, uint32_t _au_size) {
  assert(!au_size || is_empty()); 
  assert(_au_size > 0);
  assert(full_length > 0);
  clear();  
  uint32_t _num_au = ROUND_UP_TO(full_length, _au_size) / _au_size;
  au_size = _au_size;
  if( _num_au > 1 ) {
    num_au = _num_au;
    allocate();
  }
}

void bluestore_blob_use_tracker_t::get(
  uint32_t offset, uint32_t length)
{
  assert(au_size);
  if (!num_au) {
    total_bytes += length;
  }else {
    auto end = offset + length;

    while (offset < end) {
      auto phase = offset % au_size;
      bytes_per_au[offset / au_size] += 
	MIN(au_size - phase, end - offset);
      offset += (phase ? au_size - phase : au_size);
    }
  }
}

bool bluestore_blob_use_tracker_t::put(
  uint32_t offset, uint32_t length,
  PExtentVector *release_units)
{
  assert(au_size);
  if (release_units) {
    release_units->clear();
  }
  bool maybe_empty = true;
  if (!num_au) {
    assert(total_bytes >= length);
    total_bytes -= length;
  } else {
    auto end = offset + length;
    uint64_t next_offs = 0;
    while (offset < end) {
      auto phase = offset % au_size;
      size_t pos = offset / au_size;
      auto diff = MIN(au_size - phase, end - offset);
      assert(diff <= bytes_per_au[pos]);
      bytes_per_au[pos] -= diff;
      offset += (phase ? au_size - phase : au_size);
      if (bytes_per_au[pos] == 0) {
	if (release_units) {
          if (release_units->empty() || next_offs != pos * au_size) {
  	    release_units->emplace_back(pos * au_size, au_size);
          } else {
            release_units->back().length += au_size;
          }
          next_offs += au_size;
	}
      } else {
	maybe_empty = false; // micro optimization detecting we aren't empty 
	                     // even in the affected extent
      }
    }
  }
  bool empty = maybe_empty ? !is_not_empty() : false;
  if (empty && release_units) {
    release_units->clear();
  }
  return empty;
}

bool bluestore_blob_use_tracker_t::can_split() const
{
  return num_au > 0;
}

bool bluestore_blob_use_tracker_t::can_split_at(uint32_t blob_offset) const
{
  assert(au_size);
  return (blob_offset % au_size) == 0 &&
         blob_offset < num_au * au_size;
}

void bluestore_blob_use_tracker_t::split(
  uint32_t blob_offset,
  bluestore_blob_use_tracker_t* r)
{
  assert(au_size);
  assert(can_split());
  assert(can_split_at(blob_offset));
  assert(r->is_empty());
  
  uint32_t new_num_au = blob_offset / au_size;
  r->init( (num_au - new_num_au) * au_size, au_size);

  for (auto i = new_num_au; i < num_au; i++) {
    r->get((i - new_num_au) * au_size, bytes_per_au[i]);
    bytes_per_au[i] = 0;
  }
  if (new_num_au == 0) {
    clear();
  } else if (new_num_au == 1) {
    uint32_t tmp = bytes_per_au[0];
    uint32_t _au_size = au_size;
    clear();
    au_size = _au_size;
    total_bytes = tmp;
  } else {
    num_au = new_num_au;
  }
}

bool bluestore_blob_use_tracker_t::equal(
  const bluestore_blob_use_tracker_t& other) const
{
  if (!num_au && !other.num_au) {
    return total_bytes == other.total_bytes && au_size == other.au_size;
  } else if (num_au && other.num_au) {
    if (num_au != other.num_au || au_size != other.au_size) {
      return false;
    }
    for (size_t i = 0; i < num_au; i++) {
      if (bytes_per_au[i] != other.bytes_per_au[i]) {
        return false;
      }
    }
    return true;
  }

  uint32_t n = num_au ? num_au : other.num_au;
  uint32_t referenced = 
    num_au ? other.get_referenced_bytes() : get_referenced_bytes();
   auto bytes_per_au_tmp = num_au ? bytes_per_au : other.bytes_per_au;
  uint32_t my_referenced = 0;
  for (size_t i = 0; i < n; i++) {
    my_referenced += bytes_per_au_tmp[i];
    if (my_referenced > referenced) {
      return false;
    }
  }
  return my_referenced == referenced;
}

void bluestore_blob_use_tracker_t::dump(Formatter *f) const
{
  f->dump_unsigned("num_au", num_au);
  f->dump_unsigned("au_size", au_size);
  if (!num_au) {
    f->dump_unsigned("total_bytes", total_bytes);
  } else {
    f->open_array_section("bytes_per_au");
    for (size_t i = 0; i < num_au; ++i) {
      f->dump_unsigned("", bytes_per_au[i]);
    }
    f->close_section();
  }
}

void bluestore_blob_use_tracker_t::generate_test_instances(
  list<bluestore_blob_use_tracker_t*>& o)
{
  o.push_back(new bluestore_blob_use_tracker_t());
  o.back()->init(16, 16);
  o.back()->get(10, 10);
  o.back()->get(10, 5);
  o.push_back(new bluestore_blob_use_tracker_t());
  o.back()->init(60, 16);
  o.back()->get(18, 22);
  o.back()->get(20, 20);
  o.back()->get(15, 20);
}

ostream& operator<<(ostream& out, const bluestore_blob_use_tracker_t& m)
{
  out << "use_tracker(" << std::hex;
  if (!m.num_au) {
    out << "0x" << m.au_size 
        << " :"
        << "0x" << m.total_bytes;
  } else {
    out << "0x" << m.num_au 
        << "*0x" << m.au_size 
	<< " :";
    for (size_t i = 0; i < m.num_au; ++i) {
      if (i != 0)
	out << ",";
      out << "0x" << m.bytes_per_au[i];
    }
  }
  out << std::dec << ")";
  return out;
}

// bluestore_pextent_t

void bluestore_pextent_t::dump(Formatter *f) const
{
  f->dump_unsigned("offset", offset);
  f->dump_unsigned("length", length);
}

ostream& operator<<(ostream& out, const bluestore_pextent_t& o) {
  if (o.is_valid())
    return out << "0x" << std::hex << o.offset << "~" << o.length << std::dec;
  else
    return out << "!~" << std::hex << o.length << std::dec;
}

void bluestore_pextent_t::generate_test_instances(list<bluestore_pextent_t*>& ls)
{
  ls.push_back(new bluestore_pextent_t);
  ls.push_back(new bluestore_pextent_t(1, 2));
}

// bluestore_blob_t

string bluestore_blob_t::get_flags_string(unsigned flags)
{
  string s;
  if (flags & FLAG_MUTABLE) {
    s = "mutable";
  }
  if (flags & FLAG_COMPRESSED) {
    if (s.length())
      s += '+';
    s += "compressed";
  }
  if (flags & FLAG_CSUM) {
    if (s.length())
      s += '+';
    s += "csum";
  }
  if (flags & FLAG_HAS_UNUSED) {
    if (s.length())
      s += '+';
    s += "has_unused";
  }
  if (flags & FLAG_SHARED) {
    if (s.length())
      s += '+';
    s += "shared";
  }

  return s;
}

size_t bluestore_blob_t::get_csum_value_size() const 
{
  return Checksummer::get_csum_value_size(csum_type);
}

void bluestore_blob_t::dump(Formatter *f) const
{
  f->open_array_section("extents");
  for (auto& p : extents) {
    f->dump_object("extent", p);
  }
  f->close_section();
  f->dump_unsigned("compressed_length_original", compressed_length_orig);
  f->dump_unsigned("compressed_length", compressed_length);
  f->dump_unsigned("flags", flags);
  f->dump_unsigned("csum_type", csum_type);
  f->dump_unsigned("csum_chunk_order", csum_chunk_order);
  f->open_array_section("csum_data");
  size_t n = get_csum_count();
  for (unsigned i = 0; i < n; ++i)
    f->dump_unsigned("csum", get_csum_item(i));
  f->close_section();
  f->dump_unsigned("unused", unused);
}

void bluestore_blob_t::generate_test_instances(list<bluestore_blob_t*>& ls)
{
  ls.push_back(new bluestore_blob_t);
  ls.push_back(new bluestore_blob_t(0));
  ls.push_back(new bluestore_blob_t);
  ls.back()->extents.push_back(bluestore_pextent_t(111, 222));
  ls.push_back(new bluestore_blob_t);
  ls.back()->init_csum(Checksummer::CSUM_XXHASH32, 16, 65536);
  ls.back()->csum_data = buffer::claim_malloc(4, strdup("abcd"));
  ls.back()->add_unused(0, 3);
  ls.back()->add_unused(8, 8);
  ls.back()->extents.emplace_back(bluestore_pextent_t(0x40100000, 0x10000));
  ls.back()->extents.emplace_back(
    bluestore_pextent_t(bluestore_pextent_t::INVALID_OFFSET, 0x1000));
  ls.back()->extents.emplace_back(bluestore_pextent_t(0x40120000, 0x10000));
}

ostream& operator<<(ostream& out, const bluestore_blob_t& o)
{
  out << "blob(" << o.extents;
  if (o.is_compressed()) {
    out << " clen 0x" << std::hex
	<< o.compressed_length_orig
	<< " -> 0x"
	<< o.compressed_length
	<< std::dec;
  }
  if (o.flags) {
    out << " " << o.get_flags_string();
  }
  if (o.csum_type) {
    out << " " << Checksummer::get_csum_type_string(o.csum_type)
	<< "/0x" << std::hex << (1ull << o.csum_chunk_order) << std::dec;
  }
  if (o.has_unused())
    out << " unused=0x" << std::hex << o.unused << std::dec;
  out << ")";
  return out;
}

void bluestore_blob_t::calc_csum(uint64_t b_off, const bufferlist& bl)
{
  switch (csum_type) {
  case Checksummer::CSUM_XXHASH32:
    Checksummer::calculate<Checksummer::xxhash32>(
      get_csum_chunk_size(), b_off, bl.length(), bl, &csum_data);
    break;
  case Checksummer::CSUM_XXHASH64:
    Checksummer::calculate<Checksummer::xxhash64>(
      get_csum_chunk_size(), b_off, bl.length(), bl, &csum_data);
    break;;
  case Checksummer::CSUM_CRC32C:
    Checksummer::calculate<Checksummer::crc32c>(
      get_csum_chunk_size(), b_off, bl.length(), bl, &csum_data);
    break;
  case Checksummer::CSUM_CRC32C_16:
    Checksummer::calculate<Checksummer::crc32c_16>(
      get_csum_chunk_size(), b_off, bl.length(), bl, &csum_data);
    break;
  case Checksummer::CSUM_CRC32C_8:
    Checksummer::calculate<Checksummer::crc32c_8>(
      get_csum_chunk_size(), b_off, bl.length(), bl, &csum_data);
    break;
  }
}

int bluestore_blob_t::verify_csum(uint64_t b_off, const bufferlist& bl,
				  int* b_bad_off, uint64_t *bad_csum) const
{
  int r = 0;

  *b_bad_off = -1;
  switch (csum_type) {
  case Checksummer::CSUM_NONE:
    break;
  case Checksummer::CSUM_XXHASH32:
    *b_bad_off = Checksummer::verify<Checksummer::xxhash32>(
      get_csum_chunk_size(), b_off, bl.length(), bl, csum_data, bad_csum);
    break;
  case Checksummer::CSUM_XXHASH64:
    *b_bad_off = Checksummer::verify<Checksummer::xxhash64>(
      get_csum_chunk_size(), b_off, bl.length(), bl, csum_data, bad_csum);
    break;
  case Checksummer::CSUM_CRC32C:
    *b_bad_off = Checksummer::verify<Checksummer::crc32c>(
      get_csum_chunk_size(), b_off, bl.length(), bl, csum_data, bad_csum);
    break;
  case Checksummer::CSUM_CRC32C_16:
    *b_bad_off = Checksummer::verify<Checksummer::crc32c_16>(
      get_csum_chunk_size(), b_off, bl.length(), bl, csum_data, bad_csum);
    break;
  case Checksummer::CSUM_CRC32C_8:
    *b_bad_off = Checksummer::verify<Checksummer::crc32c_8>(
      get_csum_chunk_size(), b_off, bl.length(), bl, csum_data, bad_csum);
    break;
  default:
    r = -EOPNOTSUPP;
    break;
  }

  if (r < 0)
    return r;
  else if (*b_bad_off >= 0)
    return -1; // bad checksum
  else
    return 0;
}

// bluestore_shared_blob_t

void bluestore_shared_blob_t::dump(Formatter *f) const
{
  f->dump_object("ref_map", ref_map);
}

void bluestore_shared_blob_t::generate_test_instances(
  list<bluestore_shared_blob_t*>& ls)
{
  ls.push_back(new bluestore_shared_blob_t);
}

ostream& operator<<(ostream& out, const bluestore_shared_blob_t& sb)
{
  out << "shared_blob(" << sb.ref_map << ")";
  return out;
}

// bluestore_onode_t

void bluestore_onode_t::shard_info::dump(Formatter *f) const
{
  f->dump_unsigned("offset", offset);
  f->dump_unsigned("bytes", bytes);
}

ostream& operator<<(ostream& out, const bluestore_onode_t::shard_info& si)
{
  return out << std::hex << "0x" << si.offset << "(0x" << si.bytes << " bytes"
	     << std::dec << ")";
}

void bluestore_onode_t::dump(Formatter *f) const
{
  f->dump_unsigned("nid", nid);
  f->dump_unsigned("size", size);
  f->open_object_section("attrs");
  for (auto p = attrs.begin(); p != attrs.end(); ++p) {
    f->open_object_section("attr");
    f->dump_string("name", p->first.c_str());  // it's not quite std::string
    f->dump_unsigned("len", p->second.length());
    f->close_section();
  }
  f->close_section();
  f->dump_string("flags", get_flags_string());
  f->open_array_section("extent_map_shards");
  for (auto si : extent_map_shards) {
    f->dump_object("shard", si);
  }
  f->close_section();
  f->dump_unsigned("expected_object_size", expected_object_size);
  f->dump_unsigned("expected_write_size", expected_write_size);
  f->dump_unsigned("alloc_hint_flags", alloc_hint_flags);
}

void bluestore_onode_t::generate_test_instances(list<bluestore_onode_t*>& o)
{
  o.push_back(new bluestore_onode_t());
  // FIXME
}

// bluestore_wal_op_t

void bluestore_wal_op_t::dump(Formatter *f) const
{
  f->dump_unsigned("op", (int)op);
  f->dump_unsigned("data_len", data.length());
  f->open_array_section("extents");
  for (auto& e : extents) {
    f->dump_object("extent", e);
  }
  f->close_section();
}

void bluestore_wal_op_t::generate_test_instances(list<bluestore_wal_op_t*>& o)
{
  o.push_back(new bluestore_wal_op_t);
  o.push_back(new bluestore_wal_op_t);
  o.back()->op = OP_WRITE;
  o.back()->extents.push_back(bluestore_pextent_t(1, 2));
  o.back()->extents.push_back(bluestore_pextent_t(100, 5));
  o.back()->data.append("my data");
}

void bluestore_wal_transaction_t::dump(Formatter *f) const
{
  f->dump_unsigned("seq", seq);
  f->open_array_section("ops");
  for (list<bluestore_wal_op_t>::const_iterator p = ops.begin(); p != ops.end(); ++p) {
    f->dump_object("op", *p);
  }
  f->close_section();

  f->open_array_section("released extents");
  for (interval_set<uint64_t>::const_iterator p = released.begin(); p != released.end(); ++p) {
    f->open_object_section("extent");
    f->dump_unsigned("offset", p.get_start());
    f->dump_unsigned("length", p.get_len());
    f->close_section();
  }
  f->close_section();
}

void bluestore_wal_transaction_t::generate_test_instances(list<bluestore_wal_transaction_t*>& o)
{
  o.push_back(new bluestore_wal_transaction_t());
  o.push_back(new bluestore_wal_transaction_t());
  o.back()->seq = 123;
  o.back()->ops.push_back(bluestore_wal_op_t());
  o.back()->ops.push_back(bluestore_wal_op_t());
  o.back()->ops.back().op = bluestore_wal_op_t::OP_WRITE;
  o.back()->ops.back().extents.push_back(bluestore_pextent_t(1,7));
  o.back()->ops.back().data.append("foodata");
}

void bluestore_compression_header_t::dump(Formatter *f) const
{
  f->dump_unsigned("type", type);
  f->dump_unsigned("length", length);
}

void bluestore_compression_header_t::generate_test_instances(
  list<bluestore_compression_header_t*>& o)
{
  o.push_back(new bluestore_compression_header_t);
  o.push_back(new bluestore_compression_header_t(1));
  o.back()->length = 1234;
}
