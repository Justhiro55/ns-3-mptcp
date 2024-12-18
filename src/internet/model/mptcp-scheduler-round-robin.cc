#include "ns3/mptcp-scheduler-round-robin.h"
#include "ns3/mptcp-subflow.h"
#include "ns3/mptcp-socket-base.h"
#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("MpTcpSchedulerRoundRobin");

static inline SequenceNumber64 SEQ32TO64(SequenceNumber32 seq) {
  return SequenceNumber64(seq.GetValue());
}

TypeId MpTcpSchedulerRoundRobin::GetTypeId(void) {
  static TypeId tid = TypeId("ns3::MpTcpSchedulerRoundRobin")
    .SetParent<MpTcpScheduler>()
    .AddConstructor<MpTcpSchedulerRoundRobin>()
  ;
  return tid;
}

MpTcpSchedulerRoundRobin::MpTcpSchedulerRoundRobin() :
  MpTcpScheduler(),
  m_lastUsedFlowId(0),
  m_metaSock(0),
  m_cycleCount(0)  // サイクルカウンタの初期化
{
  NS_LOG_FUNCTION(this);
}

MpTcpSchedulerRoundRobin::~MpTcpSchedulerRoundRobin() {
  NS_LOG_FUNCTION(this);
}

void MpTcpSchedulerRoundRobin::SetMeta(Ptr<MpTcpSocketBase> metaSock) {
  NS_ASSERT(metaSock);
  NS_ASSERT_MSG(m_metaSock == 0, "SetMeta already called");
  m_metaSock = metaSock;
}

Ptr<MpTcpSubflow> MpTcpSchedulerRoundRobin::GetSubflowToUseForEmptyPacket() {
  NS_ASSERT(m_metaSock->GetNActiveSubflows() > 0);
  return m_metaSock->GetSubflow(0);
}

bool MpTcpSchedulerRoundRobin::GenerateMapping(int& activeSubflowArrayId, 
                                              SequenceNumber64& dsn,
                                              uint16_t& length) {
  NS_LOG_FUNCTION(this);
  NS_ASSERT(m_metaSock);

  // サブフロー数チェック
  int nbOfSubflows = m_metaSock->GetNActiveSubflows();
  if (nbOfSubflows == 0) {
    NS_LOG_DEBUG("No active subflows");
    return false;
  }

  // 送信データチェック
  SequenceNumber32 metaNextTxSeq = m_metaSock->Getvalue_nextTxSeq();
  uint32_t dataToSend = m_metaSock->m_txBuffer->SizeFromSequence(metaNextTxSeq);
  if (dataToSend == 0) {
    NS_LOG_DEBUG("No data to send");
    return false;
  }

  // DSN状態取得
  MpTcpSocketBase::DsnState& dsnState = m_metaSock->GetDsnState();
  SequenceNumber64 nextDsn = SEQ32TO64(metaNextTxSeq);

  // DSN 11以降はSlow pathからの応答を待つ
  if (nextDsn > SequenceNumber64(11) && !dsnState.waitingForSlowPath) {
    dsnState.waitingForSlowPath = true;
    NS_LOG_DEBUG("Reached DSN 11, waiting for slow path");
    return false;
  }

  // サブフロー選択 - 1:10の比率
  int subflowToUse;
  if (m_cycleCount < 10) {
    subflowToUse = 0;  // Fast path
  } else {
    subflowToUse = 1;  // Slow path
  }
  m_cycleCount = (m_cycleCount + 1) % 11;

  // 選択したサブフローのウィンドウチェック
  Ptr<MpTcpSubflow> subflow = m_metaSock->GetSubflow(subflowToUse);
  uint32_t subflowWindow = subflow->AvailableWindow();
  uint32_t metaWindow = m_metaSock->AvailableWindow();

  if (subflowWindow == 0 || metaWindow == 0) {
    NS_LOG_DEBUG("No window space available");
    return false;
  }

  // マッピング生成
  activeSubflowArrayId = subflowToUse;
  dsn = nextDsn;

  // 送信可能サイズの決定
  uint32_t maxSize = std::min({
    subflowWindow,
    metaWindow,
    dataToSend,
    subflow->GetSegSize()
  });

  length = maxSize;

  NS_LOG_DEBUG("Generated mapping for subflow " << subflowToUse 
               << " DSN=" << dsn.GetValue()
               << " len=" << length);

  return true;
}

} // namespace ns3
