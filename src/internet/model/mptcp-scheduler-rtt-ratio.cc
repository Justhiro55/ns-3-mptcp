#include "mptcp-scheduler-rtt.h"
#include "ns3/mptcp-subflow.h"
#include "ns3/mptcp-socket-base.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("MpTcpSchedulerRTT");

static inline
SequenceNumber64 SEQ32TO64(SequenceNumber32 seq) {
  return SequenceNumber64(seq.GetValue());
}

TypeId
MpTcpSchedulerRTT::GetTypeId(void) {
  static TypeId tid = TypeId("ns3::MpTcpSchedulerRTT")
    .SetParent<MpTcpScheduler>()
    .AddConstructor<MpTcpSchedulerRTT>()
  ;
  return tid;
}

MpTcpSchedulerRTT::MpTcpSchedulerRTT() :
  MpTcpScheduler(),
  m_metaSock(0),
  m_currentSubflowIndex(0),
  m_lastRttUpdate(Seconds(0)) {
  NS_LOG_FUNCTION(this);
}

MpTcpSchedulerRTT::~MpTcpSchedulerRTT() {
  NS_LOG_FUNCTION(this);
}

void
MpTcpSchedulerRTT::SetMeta(Ptr<MpTcpSocketBase> metaSock) {
  NS_ASSERT(metaSock);
  NS_ASSERT_MSG(m_metaSock == 0, "SetMeta already called");
  m_metaSock = metaSock;
}

void 
MpTcpSchedulerRTT::UpdateRTTRatios() {
  Time now = Simulator::Now();
  
  // Update RTT ratios periodically (every 100ms)
  if (now - m_lastRttUpdate < MilliSeconds(100)) {
    return;
  }
  
  m_lastRttUpdate = now;
  uint32_t nbSubflows = m_metaSock->GetNActiveSubflows();
  m_rttWeights.resize(nbSubflows);

  // Get RTTs for all subflows
  std::vector<Time> rtts(nbSubflows);
  Time minRtt = Time::Max();
  
  for (uint32_t i = 0; i < nbSubflows; i++) {
    rtts[i] = m_metaSock->GetSubflow(i)->GetRtt();
    if (rtts[i] < minRtt) {
      minRtt = rtts[i];
    }
  }

  // Calculate inverse RTT ratios 
  double totalWeight = 0;
  for (uint32_t i = 0; i < nbSubflows; i++) {
    // Weight is inversely proportional to RTT
    m_rttWeights[i] = minRtt.GetSeconds() / rtts[i].GetSeconds();
    totalWeight += m_rttWeights[i];
  }

  // Normalize weights
  for (uint32_t i = 0; i < nbSubflows; i++) {
    m_rttWeights[i] /= totalWeight;
    NS_LOG_DEBUG("Subflow " << i << " RTT=" << rtts[i].GetSeconds() 
                 << "s Weight=" << m_rttWeights[i]);
  }
}

bool
MpTcpSchedulerRTT::GenerateMapping(int& activeSubflowArrayId, SequenceNumber64& dsn, uint16_t& length) {
  NS_LOG_FUNCTION(this);
  NS_ASSERT(m_metaSock);

  UpdateRTTRatios();

  uint32_t nbSubflows = m_metaSock->GetNActiveSubflows();
  if (nbSubflows == 0) {
    return false;
  }

  // Get meta socket state
  SequenceNumber32 metaNextTxSeq = m_metaSock->Getvalue_nextTxSeq();
  uint32_t amountOfDataToSend = m_metaSock->m_txBuffer->SizeFromSequence(metaNextTxSeq);
  uint32_t metaWindow = m_metaSock->AvailableWindow();

  if (amountOfDataToSend <= 0 || metaWindow <= 0) {
    return false;
  }

  // Try subflows in order of RTT weights
  for (uint32_t i = 0; i < nbSubflows; i++) {
    m_currentSubflowIndex = (m_currentSubflowIndex + 1) % nbSubflows;
    
    // Skip if weight is too low
    if (m_rttWeights[m_currentSubflowIndex] < 0.1) {
      continue;
    }

    Ptr<MpTcpSubflow> subflow = m_metaSock->GetSubflow(m_currentSubflowIndex);
    uint32_t subflowWindow = subflow->AvailableWindow();
    
    // Calculate how much we can send on this subflow
    uint32_t maxToSend = std::min(subflowWindow, metaWindow);
    maxToSend = std::min(maxToSend, amountOfDataToSend);

    if (maxToSend > 0) {
      activeSubflowArrayId = m_currentSubflowIndex;
      dsn = SEQ32TO64(metaNextTxSeq);
      length = std::min(maxToSend, subflow->GetSegSize());

      NS_LOG_DEBUG("Scheduled " << length << " bytes on subflow " 
                   << m_currentSubflowIndex << " (weight=" 
                   << m_rttWeights[m_currentSubflowIndex] << ")");
      return true;
    }
  }

  return false;
}

Ptr<MpTcpSubflow>
MpTcpSchedulerRTT::GetSubflowToUseForEmptyPacket() {
  NS_ASSERT(m_metaSock->GetNActiveSubflows() > 0);
  
  // Use subflow with lowest RTT for control packets
  uint32_t nbSubflows = m_metaSock->GetNActiveSubflows();
  uint32_t bestIndex = 0;
  Time bestRtt = Time::Max();
  
  for (uint32_t i = 0; i < nbSubflows; i++) {
    Time rtt = m_metaSock->GetSubflow(i)->GetRtt();
    if (rtt < bestRtt) {
      bestRtt = rtt;
      bestIndex = i; 
    }
  }
  
  return m_metaSock->GetSubflow(bestIndex);
}

} // namespace ns3
