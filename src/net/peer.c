/*
 * Copyright (C) 2026 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * You can also choose to distribute this program under the terms of
 * the Unmodified Binary Distribution Licence (as given in the file
 * COPYING.UBDL), provided that you have satisfied its requirements.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );
FILE_SECBOOT ( PERMITTED );

/** @file
 *
 * Peer-to-peer download abstraction
 *
 * We define a concept of a peer-to-peer file download operation as
 * comprising a fixed list of variable-length file segments that must
 * be downloaded and a variable set of peers from which we may attempt
 * to retrieve each segment.
 *
 * We assume that some kind of discovery mechanism exists to detect
 * peers, but that this operates at a much slower timescale than the
 * retrieval mechanism.  We expect to perform only a handful of
 * discoveries per thousand segments, and we assume that we have
 * imperfect information about which peers may hold each segment.
 *
 * (This maps well onto the way that PeerDist/BranchCache must be used
 * in practice: in theory a PeerDist client should perform discovery
 * for every segment, but doing so drops throughput by three orders of
 * magnitude and so clients must instead optimistically attempt to
 * retrieve segments from existing peers.)
 *
 * For each peer, we maintain the following properties:
 *
 *   - p[i] ("probability"): the probability that an attempt to
 *     retrieve a previously unattempted segment from this peer will
 *     succeed
 *
 *   - r[i] ("retry"): the probability that an attempt to retrieve a
 *     previously attempted (but failed) segment from this peer will
 *     succeed
 *
 *   - h[i] ("hit time"): the expected elapsed time until the first
 *     byte of a successful retrieval response from this peer
 *
 *   - b[i] ("bandwidth"): the expected throughput for a success
 *     response from this peer
 *
 *   - v[i] ("variation"): the expected absolute difference between
 *     the estimated total success response time (calculated from h[i]
 *     and b[i]) and the measured total success response time
 *
 *   - m[i] ("miss time"): the expected elapsed time until a failure
 *     response from this peer
 *
 *   - n[i] ("number"): the number of currently active retrieval
 *     attempts from this peer by this client
 *
 *   - f[i] ("failure"): the number of hard failures (excluding cache
 *     misses) experienced when attempting retrieval from this peer
 *
 *   - k[i] ("scale"): the policy-defined relative cost of attempting
 *     retrieval from this peer
 *
 * Several of these values are estimated using an exponentially
 * weighted moving average estimator.  For example, the probability
 * estimate p[i] will be updated towards 1.0 for each successful
 * retrieval and updated towards 0.0 for each unsuccessful retrieval.
 *
 * We define groups of peers that are expected to share similar
 * characteristics.  For example: peers discovered via multicast on a
 * local network may be in one group, with predefined (and potentially
 * remote) peers placed into a separate group.  Some properties are
 * maintained as per-group estimates rather than individual per-peer
 * estimates (or as combinations of group and individual estimates),
 * to counteract the low expected sample count for any individual peer
 * within the group.
 *
 * The set of peers may include some special-function peers, such as a
 * local filesystem cache (which will be extremely fast for any
 * segments that it happens to hold), or an origin server (which may
 * be extremely slow but is guaranteed to hold all segments).  These
 * are modelled as peer groups with suitable characteristics (e.g. a
 * high k[i] cost scale factor for an origin server).
 *
 * For each segment within the file, we select a peer and attempt
 * retrieval from that peer.  If the retrieval fails, then we continue
 * selecting and attempting peers until the retrieval of that segment
 * succeeds (or until all possible attempts have been exhausted).
 *
 * We allow for the possibility that peers may report a false miss as
 * a crude load-balancing mechanism (or, equivalently, that peers may
 * obtain segments that they did not previously hold), by allowing for
 * up to a fixed number of retries from each peer.
 *
 * We must maintain a per-peer counter for each segment to track how
 * many times we have already attempted retrieval of that segment from
 * that peer.  To avoid the requirement to dynamically allocate
 * storage for each {segment, peer} combination, we place a fixed
 * upper limit on the total number of recorded peers, allocate each
 * peer a unique peer index within this range, and use a fixed-size
 * per-segment array to record the number of retrieval attempts made
 * for each peer index.
 *
 */

