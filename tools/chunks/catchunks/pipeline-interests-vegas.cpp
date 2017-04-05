/**
 * Copyright (c) 2016,  Regents of the University of California,
 *                      Colorado State University,
 *                      University Pierre & Marie Curie, Sorbonne University.
 *
 * This file is part of ndn-tools (Named Data Networking Essential Tools).
 * See AUTHORS.md for complete list of ndn-tools authors and contributors.
 *
 * ndn-tools is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * ndn-tools is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ndn-tools, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 *
 * See AUTHORS.md for complete list of ndn-cxx authors and contributors.
 *
 * @author Shuo Yang
 */

#include "pipeline-interests-vegas.hpp"

#include <cmath>

namespace ndn {
namespace chunks {
namespace vegas {

PipelineInterestsVegas::PipelineInterestsVegas(Face& face,
                                           RttEstimator& rttEstimator,
                                           RateEstimator& rateEstimator,
                                           const Options& options)
  : PipelineInterests(face)
  , m_options(options)
  , m_rttEstimator(rttEstimator)
  , m_rateEstimator(rateEstimator)
  , m_scheduler(m_face.getIoService())
  , m_nextSegmentNo(0)
  , m_receivedSize(0)
  , m_highData(0)
  , m_highInterest(0)
  , m_recPoint(0)
  , m_nInFlight(0)
  , m_nReceived(0)
  , m_nLossEvents(0)
  , m_nRetransmitted(0)
  , m_cwnd(m_options.initCwnd)
  , m_ssthresh(m_options.initSsthresh)
  , m_hasFailure(false)
  , m_failedSegNo(0)
  , m_nPackets(0)
  , m_nBits(0)
  , m_inSlowStart(true)
  , m_baseRtt(std::numeric_limits<double>::quiet_NaN())
{
  if (m_options.isVerbose) {
    std::cerr << m_options;
  }
}

PipelineInterestsVegas::~PipelineInterestsVegas()
{
  cancel();
}

void
PipelineInterestsVegas::doRun()
{
  // record the start time of running pipeline
  m_startTime = time::steady_clock::now();

  // count the excluded segment
  m_nReceived++;

  // schedule the event to check rate at rate interval timer
  m_scheduler.scheduleEvent(time::milliseconds((int) (m_options.rateInterval * 1000)),
                            [this] {checkRate();});

  sendInterest(getNextSegmentNo(), false);
}

void
PipelineInterestsVegas::doCancel()
{
  for (const auto& entry : m_segmentInfo) {
    const SegmentInfo& segInfo = entry.second;
    m_face.removePendingInterest(segInfo.interestId);
  }
  m_segmentInfo.clear();
  m_scheduler.cancelAllEvents();
  m_face.getIoService().stop();
}

void PipelineInterestsVegas::checkRate()
{
  if (isStopping())
    return;

  time::steady_clock::duration cur = time::steady_clock::now() - m_startTime;
  double now = (double) cur.count() / 1000000000;

  m_rateEstimator.addMeasurement(now, m_nPackets, m_nBits);

  m_nPackets = 0;
  m_nBits = 0;
  m_scheduler.scheduleEvent(time::milliseconds((int) (m_options.rateInterval * 1000)),
                            [this] {checkRate();});

}

void
PipelineInterestsVegas::sendInterest(uint64_t segNo, bool isRetransmission)
{
  if (isStopping())
    return;

  if (m_hasFinalBlockId && segNo > m_lastSegmentNo)
    return;

  if (!isRetransmission && m_hasFailure)
    return;

  if (m_options.isVerbose) {
    if (isRetransmission)
      std::cerr << "Retransmitting segment #" << segNo << std::endl;
    else
      std::cerr << "Requesting segment #" << segNo << std::endl;
  }

  if (isRetransmission) {
    m_retxCount[segNo] += 1; // entry for segNo has been initialized in updateRetxQueue()
    if (m_retxCount[segNo] > m_options.maxRetriesOnTimeoutOrNack) {
      return handleFail(segNo, "Reached the maximum number of retries (" +
                        to_string(m_options.maxRetriesOnTimeoutOrNack) +
                        ") while retrieving segment #" + to_string(segNo));
    }

    if (m_options.isVerbose) {
      std::cerr << "# of retries for segment #" << segNo
                << " is " << m_retxCount[segNo] << std::endl;
    }

    m_face.removePendingInterest(m_segmentInfo[segNo].interestId);
  }

  Interest interest(Name(m_prefix).appendSegment(segNo));
  interest.setInterestLifetime(m_options.interestLifetime);
  interest.setMustBeFresh(m_options.mustBeFresh);
  interest.setMaxSuffixComponents(1);

  auto interestId = m_face.expressInterest(interest,
                                           bind(&PipelineInterestsVegas::handleData, this, _1, _2),
                                           bind(&PipelineInterestsVegas::handleNack, this, _1, _2),
                                           bind(&PipelineInterestsVegas::handleLifetimeExpiration,
                                                this, _1));

  m_nInFlight++;

  if (isRetransmission) {
    SegmentInfo& segInfo = m_segmentInfo[segNo];
    segInfo.state = SegmentState::Retransmitted;
    segInfo.rto = m_rttEstimator.getEstimatedRto();
    segInfo.timeSent = time::steady_clock::now();
    segInfo.interestId = interestId;
    m_nRetransmitted++;
  }
  else {
    m_highInterest = segNo;
    Milliseconds rto = m_rttEstimator.getEstimatedRto();
    SegmentInfo segInfo{interestId, SegmentState::FirstTimeSent, rto, time::steady_clock::now()};

    m_segmentInfo.emplace(segNo, segInfo);
  }
}

void
PipelineInterestsVegas::schedulePackets()
{
  int availableWindowSize = static_cast<int>(m_cwnd) - m_nInFlight;
  while (availableWindowSize > 0) {
    uint64_t nextSegNo = getNextSegmentNo();

    if (m_hasFinalBlockId && nextSegNo > m_lastSegmentNo) {
      // scan through segmentInfo map to put unacked Interests to retxQueue
      // after all the in-order segments were sent
      updateRetxQueue();
    }

    if (m_hasFinalBlockId && nextSegNo <= m_lastSegmentNo) {
      // in-order segment takes higher priority than those need retransmission
      sendInterest(nextSegNo, false);
    } else if (!m_retxQueue.empty()) { // do retransmission once all the in-order segments were sent
      uint64_t retxSegNo = m_retxQueue.front();
      m_retxQueue.pop();

      auto it = m_segmentInfo.find(retxSegNo);
      if (it == m_segmentInfo.end()) {
        continue;
      }
      // the segment is still in the map, it means that it needs to be retransmitted
      sendInterest(retxSegNo, true);
    }
    availableWindowSize--;
  }
}

void
PipelineInterestsVegas::updateRetxQueue()
{
  if (isStopping())
    return;

  int timeoutCount = 0;

  for (auto& entry : m_segmentInfo) {
    SegmentInfo& segInfo = entry.second;
    if (segInfo.state == SegmentState::FirstTimeSent ||
        segInfo.state == SegmentState::Retransmitted) {
      Milliseconds timeElapsed = time::steady_clock::now() - segInfo.timeSent;
      if (timeElapsed.count() > segInfo.rto.count()) { // timer expired?
        uint64_t timedoutSeg = entry.first;
        m_retxQueue.push(timedoutSeg); // put on retx queue

        // initialize the retx count to 0 for segments whose state is FirstTimeSent
        if (segInfo.state == SegmentState::FirstTimeSent)
          m_retxCount.insert(std::make_pair(timedoutSeg, 0));

        segInfo.state = SegmentState::InRetxQueue; // update status
        timeoutCount++;
      }
    }
  }

  if (m_nInFlight > static_cast<uint64_t>(timeoutCount))
    m_nInFlight -= timeoutCount;
  else
    m_nInFlight = 0;

  if (timeoutCount > 0) {
    std::cerr << "There are " << timeoutCount << " segments timed out...\n";
  }
}

void
PipelineInterestsVegas::handleData(const Interest& interest, const Data& data)
{
  if (isStopping())
    return;

  // Data name will not have extra components because MaxSuffixComponents is set to 1
  BOOST_ASSERT(data.getName().equals(interest.getName()));

  if (!m_hasFinalBlockId && !data.getFinalBlockId().empty()) {
    m_lastSegmentNo = data.getFinalBlockId().toSegment();
    m_hasFinalBlockId = true;
    cancelInFlightSegmentsGreaterThan(m_lastSegmentNo);
    if (m_hasFailure && m_lastSegmentNo >= m_failedSegNo) {
      // previously failed segment is part of the content
      return onFailure(m_failureReason);
    } else {
      m_hasFailure = false;
    }
  }

  uint64_t recvSegNo = data.getName()[-1].toSegment();
  SegmentInfo& segInfo = m_segmentInfo[recvSegNo];

  auto it = m_segmentInfo.find(recvSegNo);
  if (it == m_segmentInfo.end()) {
    if (m_options.isVerbose) {
      std::cerr << "Segment #" << recvSegNo << " has been received already\n";
    }
    return;
  }

  // measure rate
  m_nPackets += 1;
  m_nBits += data.getContent().size() * 8;

  Milliseconds rtt = time::steady_clock::now() - segInfo.timeSent;

  if (m_options.isVerbose) {
    std::cerr << "Received segment #" << recvSegNo
              << ", rtt=" << rtt.count() << "ms"
              << ", rto=" << segInfo.rto.count() << "ms"
              << ", segment state: " << segInfo.state
              << std::endl;
  }

  // for segments in retransmission queue, no need to decrement m_nInFlight since
  // it's already been decremented when segments timed out
  if (segInfo.state != SegmentState::InRetxQueue && m_nInFlight > 0) {
    m_nInFlight--;
  }

  m_receivedSize += data.getContent().value_size();
  m_nReceived++;

  if (segInfo.state == SegmentState::FirstTimeSent ||
      (segInfo.state == SegmentState::InRetxQueue &&
       m_retxCount[recvSegNo] == 0)) { // do not sample RTT for retransmitted segments
    size_t nExpectedSamples = std::max(static_cast<int>(std::ceil(m_nInFlight / 2.0)), 1);

    time::steady_clock::duration cur = time::steady_clock::now() - m_startTime;
    double now = (double) cur.count() / 1000000000;
    m_rttEstimator.addMeasurement(recvSegNo, now, rtt, nExpectedSamples);
  }
  m_segmentInfo.erase(recvSegNo); // remove the entry associated with the received segment

  // congestion control
  if (m_inSlowStart) {
    if (std::isnan(m_baseRtt.count())) { // first measurement
      m_baseRtt = rtt;
    } else {
      m_baseRtt = std::min(m_baseRtt, rtt);
    }
    // if RTT of the received segment exceeds its RTO,
    // leave the slow start phase, enter vegas congestion avoidance phase.
    //if (rtt >= segInfo.rto) {
    if (rtt >= m_baseRtt * 1.1) {
      m_inSlowStart = false;
      m_ssthresh = std::max(2.0, m_cwnd * 0.8);
      m_cwnd = m_ssthresh;
      afterCwndChange(time::steady_clock::now() - m_startTime, m_cwnd);

      m_rttEstimator.backoffRto();
      m_nLossEvents++;

      if (m_options.isVerbose) {
      }
      std::cerr << "Leaving slow start with cwnd  = " << m_cwnd
                << " and ssthresh = " << m_ssthresh << std::endl;

      // start a new vegas epoch and schedule the event for Vegas update after the epoch
      m_vegasEpochDuration = std::max(m_baseRtt, m_options.minVegasEpochDuration);
      m_cwndInVegasEpoch = m_cwnd;
      m_nReceivedInVegasEpoch = 0;
      // m_minRttInVegasEpoch = boost::chrono::milliseconds(0);
      m_minRttInVegasEpoch = time::milliseconds(0);
      m_scheduler.scheduleEvent(time::milliseconds((int) (m_vegasEpochDuration.count())),
                                [this] { vegasUpdate(); });
    } else {
      if (m_cwnd < m_ssthresh) {
        m_cwnd += 1; // additive increase
        afterCwndChange(time::steady_clock::now() - m_startTime, m_cwnd);
      }
    }
  } else { // in Vegas Epoch
    m_nReceivedInVegasEpoch++;
    // update m_minRttInVegasEpoch based on measured rtt within this epoch
    if (m_minRttInVegasEpoch.count() == 0) {
      m_minRttInVegasEpoch = rtt;
    } else {
      if (segInfo.state == SegmentState::FirstTimeSent ||
          (segInfo.state == SegmentState::InRetxQueue &&
           m_retxCount[recvSegNo] == 0)) {
        m_minRttInVegasEpoch = std::min(m_minRttInVegasEpoch, rtt);
      }
    }
  }

  onData(interest, data);

  BOOST_ASSERT(m_nReceived > 0);
  if (m_hasFinalBlockId && m_nReceived - 1 >= m_lastSegmentNo) { // all segments have been received
    cancel();
    if (m_options.isVerbose || m_options.outputSummary) {
      printSummary();
    }
  }
  else {
    schedulePackets();
  }
}

void
PipelineInterestsVegas::vegasUpdate()
{
  double expected_rate = m_cwndInVegasEpoch * 1000 / m_vegasEpochDuration.count();
  double actual_rate = m_nReceivedInVegasEpoch * 1000 / m_vegasEpochDuration.count();
  double diff_rate = expected_rate - actual_rate;

  if (false) {
    std::cerr << "Vegas Updating with baseRTT=" << m_baseRtt << ", cwnd=" << m_cwnd
              << ", expected=" << expected_rate << ", actual=" << actual_rate << std::endl;
  }

  if (diff_rate >= 0 && diff_rate <= m_options.vegasGamma) {
    if (diff_rate < m_options.vegasAlpha) {
      m_cwnd += 1; // linear increase
    } else if (diff_rate > m_options.vegasBeta) {
      m_cwnd = std::max(2.0, m_cwnd - 1); // linear decrease
    } else {
      // keep cwnd unchanged
    }
  } else if (diff_rate > m_options.vegasGamma) { // adjust base RTT to larger value
    if (m_minRttInVegasEpoch > m_baseRtt) {
      m_baseRtt = m_minRttInVegasEpoch;
    }
  } else if (diff_rate < 0) { // adjust base RTT to smaller value
    if (m_minRttInVegasEpoch < m_baseRtt) {
      m_baseRtt = m_minRttInVegasEpoch;
    }
    m_cwnd += 1; // linear increase
  }

  if (false) {
    std::cerr << "Vegas Updated with baseRTT=" << m_baseRtt << ", cwnd=" << m_cwnd
              << ", m_nReceived=" << m_nReceived << std::endl;
  }

  afterCwndChange(time::steady_clock::now() - m_startTime, m_cwnd);

  // start a new vegas epoch and schedule the event for Vegas update after the epoch
  m_vegasEpochDuration = std::max(m_baseRtt, m_options.minVegasEpochDuration);
  m_cwndInVegasEpoch = m_cwnd;
  m_nReceivedInVegasEpoch = 0;
  m_minRttInVegasEpoch = time::milliseconds(0);
  m_scheduler.scheduleEvent(time::milliseconds((int) (m_vegasEpochDuration.count())),
                            [this] { vegasUpdate(); });
}

void
PipelineInterestsVegas::handleNack(const Interest& interest, const lp::Nack& nack)
{
  if (isStopping())
    return;

  if (m_options.isVerbose)
    std::cerr << "Received Nack with reason " << nack.getReason()
              << " for Interest " << interest << std::endl;

  uint64_t segNo = interest.getName()[-1].toSegment();

  switch (nack.getReason()) {
    case lp::NackReason::DUPLICATE: {
      break; // ignore duplicates
    }
    case lp::NackReason::CONGESTION: { // treated the same as timeout for now
      break;
    }
    default: {
      handleFail(segNo, "Could not retrieve data for " + interest.getName().toUri() +
                 ", reason: " + boost::lexical_cast<std::string>(nack.getReason()));
      break;
    }
  }
}

void
PipelineInterestsVegas::handleLifetimeExpiration(const Interest& interest)
{
  std::cerr << "Interest lifetime expired\n";

  if (isStopping())
    return;

  uint64_t segNo = interest.getName()[-1].toSegment();
  m_retxQueue.push(segNo); // put on retx queue
  m_segmentInfo[segNo].state = SegmentState::InRetxQueue; // update state

  if (m_nInFlight > 0)
    m_nInFlight -= 1;

  schedulePackets();
  return;
}

void
PipelineInterestsVegas::handleFail(uint64_t segNo, const std::string& reason)
{
  if (isStopping())
    return;

  // if the failed segment is definitely part of the content, raise a fatal error
  if (m_hasFinalBlockId && segNo <= m_lastSegmentNo)
    return onFailure(reason);

  if (!m_hasFinalBlockId) {
    m_segmentInfo.erase(segNo);
    if (m_nInFlight > 0)
      m_nInFlight--;

    if (m_segmentInfo.empty()) {
      onFailure("Fetching terminated but no final segment number has been found");
    }
    else {
      cancelInFlightSegmentsGreaterThan(segNo);
      m_hasFailure = true;
      m_failedSegNo = segNo;
      m_failureReason = reason;
    }
  }
}

uint64_t
PipelineInterestsVegas::getNextSegmentNo()
{
  // get around the excluded segment
  if (m_nextSegmentNo == m_excludedSegmentNo)
    m_nextSegmentNo++;
  return m_nextSegmentNo++;
}

void
PipelineInterestsVegas::cancelInFlightSegmentsGreaterThan(uint64_t segmentNo)
{
  for (auto it = m_segmentInfo.begin(); it != m_segmentInfo.end();) {
    // cancel fetching all segments that follow
    if (it->first > segmentNo) {
      m_face.removePendingInterest(it->second.interestId);
      it = m_segmentInfo.erase(it);
      if (m_nInFlight > 0)
        m_nInFlight--;
    }
    else {
      ++it;
    }
  }
}

void
PipelineInterestsVegas::increaseWindow()
{
  afterCwndChange(time::steady_clock::now() - m_startTime, m_cwnd);
}

void
PipelineInterestsVegas::decreaseWindow()
{
  afterCwndChange(time::steady_clock::now() - m_startTime, m_cwnd);
}

void
PipelineInterestsVegas::printSummary() const
{
  Milliseconds timePassed = time::steady_clock::now() - m_startTime;
  double throughput = (8 * m_receivedSize * 1000) / timePassed.count();

  int pow = 0;
  std::string throughputUnit;
  while (throughput >= 1000.0 && pow < 4) {
    throughput /= 1000.0;
    pow++;
  }
  switch (pow) {
    case 0:
      throughputUnit = "bit/s";
      break;
    case 1:
      throughputUnit = "kbit/s";
      break;
    case 2:
      throughputUnit = "Mbit/s";
      break;
    case 3:
      throughputUnit = "Gbit/s";
      break;
    case 4:
      throughputUnit = "Tbit/s";
      break;
  }

  std::cerr << "\nAll segments have been received.\n"
            << "Total # of segments received: " << m_nReceived << "\n"
            << "Time used: " << timePassed.count() << " ms" << "\n"
            << "Base RTT: " << m_baseRtt << "\n"
            << "\tVegas alpha = " << m_options.vegasAlpha << "\n"
            << "\tVegas beta = " << m_options.vegasBeta << "\n"
            << "\tVegas gamma = " << m_options.vegasGamma << "\n"
            << "Total # of retransmitted segments: " << m_nRetransmitted << "\n"
            << "Goodput: " << throughput << " " << throughputUnit << "\n";
}

std::ostream&
operator<<(std::ostream& os, const PipelineInterestsVegasOptions& options)
{
  os << "Pipeline basic parameters:\n"
     << "\tMax retries on timeout or Nack = " << options.maxRetriesOnTimeoutOrNack << "\n"
     << "\tonly return fresh content? = " << std::boolalpha << options.mustBeFresh << "\n"
     << "\tInterest life time = " << options.interestLifetime << "\n"
     << "\tis verbose? = " << std::boolalpha << options.isVerbose << "\n";

  os << "PipelineInterestsVegas initial parameters:" << "\n"
     << "\tprint summary? = " << std::boolalpha << options.outputSummary << "\n"
     << "\tInitial congestion window size = " << options.initCwnd << "\n"
     << "\tInitial slow start threshold = " << options.initSsthresh << "\n"
     << "\tRate check interval = " << options.rateInterval << " second(s)\n"
     << "\tVegas alpha = " << options.vegasAlpha << "\n"
     << "\tVegas beta = " << options.vegasBeta << "\n"
     << "\tVegas gamma = " << options.vegasGamma << "\n"
     << "\tMin Vegas Epoch Duartion = " << options.minVegasEpochDuration << "\n";

  std::string cwaStatus = options.disableCwa ? "disabled" : "enabled";
  os << "\tConservative Window Adaptation " << cwaStatus << "\n";

  std::string cwndStatus = options.resetCwndToInit ? "initCwnd" : "ssthresh";
  os << "\tResetting cwnd to " << cwndStatus << " when loss event occurs" << "\n";

  return os;
}

} // namespace vegas
} // namespace chunks
} // namespace ndn
