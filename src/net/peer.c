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
 *   - w[i] ("weight"): the policy-defined relative cost weighting of
 *     attempting retrieval from this peer
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
 * high cost weighting w[i] for an origin server).
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
 * For a segment of length L, we can estimate the time T to complete a
 * retrieval attempt as:
 *
 *   T[i] = { h[i] + L/b[i]	if the retrieval is successful
 *          { m[i]		if the retrieval is not successful
 *
 * Define q[i] as the relevant probability that the retrieval will be
 * successful (i.e. q[i]=p[i] if we have not previously attempted
 * retrieval of this segment from this peer, otherwise q[i]=r[i]).
 * Then the expected time to complete a retrieval attempt (whether
 * successful or not) is:
 *
 *   E[i] = q[i]*(h[i] + L/b[i]) + (1-q[i])*m[i]
 *
 * Define the cost C[i] of a retrieval attempt to be the expected time
 * E[i] multiplied by the policy-defined relative cost weighting w[i]:
 *
 *   C[i] = w[i]*E[i]
 *        = w[i]*(q[i]*(h[i] + L/b[i]) + (1-q[i])*m[i])
 *
 * Consider an ordering strategy {i_1, i_2, ...} in which each peer
 * will be attempted in turn until the first attempt that succeeds in
 * retrieving the segment.  The expected cost of executing this
 * strategy is then:
 *
 *   C = Σ{k} C[i_k] * ∏{j<k} (1-q[i_k])
 *
 * We can minimise this expected cost using an algorithm equivalent to
 * a bubble sort.  Consider two peers x and y that are currently
 * ordered adjacently within the strategy, and then consider the
 * effect on the cost if the order of these two peers were to be
 * swapped.  Multiplication of the (1-q[x])(1-q[y]) terms is
 * commutative, and so all preceding and all following terms within
 * the summation would be unaffected by the swap and may be
 * disregarded for the purpose of comparison.  For the two terms that
 * remain, the product of all preceding terms within the product is
 * identical and may be similarly factored out and disregarded.  We
 * are therefore left to compare only:
 *
 *   C[x] + C[y]*(1-q[x])  <=>  C[y] + C[x]*(1-q[y])
 *
 *              C[x]*q[y]  <=>  C[y]*q[x]
 *
 *              C[x]/q[x]  <=>  C[y]/q[y]
 *
 * This gives us a peer score k[i]=C[i]/q[i] for each peer that we can
 * use as a sort key.  By application of the standard bubble sort
 * algorithm, we know that we can minimise the overall expected cost
 * if we sort peers in ascending order of peer score k[i].
 *
 * We never need to construct the full sorted list.  At each decision
 * point, the question is simply which peer to attempt next.  The
 * minimal expected cost is obtained when peers are sorted in
 * ascending order of k[i], and so the best peer to try next is the
 * peer with the lowest peer score k[i].  Expanding:
 *
 *   k[i] = C[i]/q[i]
 *        = w[i]*(q[i]*(h[i] + L/b[i]) + (1-q[i])*m[i]) / q[i]
 *        = w[i]*(h[i] + L/b[i] + m[i]*((1-q[i])/q[i]))
 *
 * We introduce some deliberate suboptimality to avoid clustering and
 * herding behaviours between multiple clients all concurrently
 * attempting the same download from the same set of available peers.
 *
 * Firstly, the policy-defined cost weighting w[i] is weakly
 * randomised to provide a light peer preference that differs between
 * clients but is consistent for each client.  In the case of a set of
 * identically performing peers, this acts to distribute clients
 * across multiple peers.
 *
 * Secondly, we do not always choose the best peer (i.e. the peer with
 * the lowest peer score).  Instead we randomly choose from amongst
 * the subset of peers that have a score within a fixed multiple of
 * the lowest peer score.  Where scores are widely varying (e.g. a
 * local filesystem cache versus a LAN peer), this will cause us to
 * always choose the better scoring peer.  Where scores are similar,
 * this allows us to statistically balance load amongst multiple
 * comparable peers.
 *
 * The remaining peer properties are used to support aspects of
 * operation that are not directly related to the peer selection
 * algorithm.  For example, peers may be evicted from the peer list
 * after a certain number of hard failures, and the estimated
 * variation may be used to calculate an appropriate timeout for a
 * retrieval attempt.
 *
 * The peer-to-peer download abstraction is modelled on the practical
 * operation of the PeerDist protocol, but is designed to be passive
 * and independent of the actual wire protocol.  It provides the
 * operations that support the peer selection algorithm, but does not
 * itself initiate or process any network traffic.
 *
 */
