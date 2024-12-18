#ifndef MPTCP_SCHEDULER_ROUND_ROBIN_H
#define MPTCP_SCHEDULER_ROUND_ROBIN_H

#include "ns3/mptcp-scheduler.h"
#include "ns3/object.h"
#include "ns3/ptr.h"
#include <vector>
#include <queue>

namespace ns3 {

class MpTcpSocketBase;
class MpTcpSubflow;

class MpTcpSchedulerRoundRobin : public MpTcpScheduler {
public:
  static TypeId GetTypeId(void);

  MpTcpSchedulerRoundRobin();
  virtual ~MpTcpSchedulerRoundRobin();
  void SetMeta(Ptr<MpTcpSocketBase> metaSock);

  virtual bool GenerateMapping(int& activeSubflowArrayId, SequenceNumber64& dsn, uint16_t& length);
  virtual Ptr<MpTcpSubflow> GetSubflowToUseForEmptyPacket();

protected:
  uint8_t m_lastUsedFlowId;
  Ptr<MpTcpSocketBase> m_metaSock;

  // RTTベースのスケジューリング用の変数
  uint32_t m_cycleCount;  // 現在のサイクル数
  static const uint32_t TOTAL_CYCLES = 11; // 1:10の比率のための総サイクル数
  uint32_t m_pendingFastPathPackets; // 送信待ちの低遅延パスのパケット数
  
  bool m_slowPathActive; // 高遅延パスがアクティブか
  bool m_waitingForSlowPath; // 高遅延パスの到着待ちか

  void ResetCycle();
  bool CanSendOnFastPath() const;
};

} // namespace ns3

#endif /* MPTCP_SCHEDULER_ROUND_ROBIN_H */
