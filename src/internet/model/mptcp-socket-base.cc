/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2015 University of Sussex
 * Copyright (c) 2015 Université Pierre et Marie Curie (UPMC)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author:  Kashif Nadeem <kshfnadeem@gmail.com>
 *          Matthieu Coudron <matthieu.coudron@lip6.fr>
 *          Morteza Kheirkhah <m.kheirkhah@sussex.ac.uk>
 */

#include <algorithm>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <map>
#include "ns3/abort.h"
#include "ns3/log.h"
#include "ns3/enum.h"
#include "ns3/string.h"
#include "ns3/tcp-l4-protocol.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/error-model.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/pointer.h"
#include "ns3/drop-tail-queue.h"
#include "ns3/object-vector.h"
#include "ns3/mptcp-scheduler-round-robin.h"
#include "ns3/mptcp-socket-base.h"
#include "ns3/mptcp-subflow.h"
#include "ns3/tcp-option-mptcp.h"
#include "ns3/callback.h"
#include "ns3/trace-helper.h"
#include "ns3/mptcp-ndiffports.h"
#include "ns3/mptcp-fullmesh.h"

using namespace std;

namespace ns3
{
NS_LOG_COMPONENT_DEFINE("MpTcpSocketBase");

NS_OBJECT_ENSURE_REGISTERED(MpTcpSocketBase); 

static inline
SequenceNumber32 SEQ64TO32(SequenceNumber64 seq)
{
  return SequenceNumber32( seq.GetValue());
}

static inline
SequenceNumber64 SEQ32TO64(SequenceNumber32 seq)
{
  return SequenceNumber64( seq.GetValue());
}

//! wrapper function
static inline
MpTcpMapping
GetMapping(Ptr<TcpOptionMpTcpDSS> dss)
{
  MpTcpMapping mapping;
  uint64_t dsn;
  uint32_t ssn;
  uint16_t length;

  dss->GetMapping (dsn, ssn, length);
  mapping.SetHeadDSN( SequenceNumber64(dsn));
  mapping.SetMappingSize(length);
  mapping.MapToSSN( SequenceNumber32(ssn));
  return mapping;
}

TypeId
MpTcpSocketBase::GetTypeId(void)
{
  static TypeId tid = TypeId ("ns3::MpTcpSocketBase")
      .SetParent<TcpSocketBase>()
      .SetGroupName ("Internet")
      .AddConstructor<MpTcpSocketBase>()
      .AddAttribute ("SocketType",
               "Socket type of TCP objects.",
               TypeIdValue (MpTcpSubflow::GetTypeId ()),
               MakeTypeIdAccessor (&MpTcpSocketBase::m_subflowTypeId),
               MakeTypeIdChecker ())
      .AddAttribute ("Scheduler",
               "How to generate the mappings",
               TypeIdValue (MpTcpScheduler::GetTypeId ()),
               MakeTypeIdAccessor (&MpTcpSocketBase::m_schedulerTypeId),
               MakeTypeIdChecker ())
     .AddAttribute("PathManagerMode",
              "Mechanism for establishing new sub-flows",
              EnumValue (MpTcpSocketBase::FullMesh),
              MakeEnumAccessor (&MpTcpSocketBase::m_pathManager),
              MakeEnumChecker (MpTcpSocketBase::Default,"Default",
                               MpTcpSocketBase::FullMesh, "FullMesh",
                               MpTcpSocketBase::nDiffPorts, "nDiffPorts"))

  ;
  return tid;
}

static const std::string containerNames[MpTcpSocketBase::Maximum] = {
  "Established",
  "Others",
  "Closing"
};

MpTcpSocketBase::MpTcpSocketBase(const TcpSocketBase& sock)
  : TcpSocketBase(sock),
    m_peerToken(0),
    m_peerKey(0),
    m_doChecksum(false),
    m_receivedDSS(false),
    m_multipleSubflows(false),
    m_lastProcessedDack(0)
{
  NS_LOG_FUNCTION(this);
  NS_LOG_LOGIC("Copying from TcpSocketBase");
  CreateScheduler(m_schedulerTypeId);
}

MpTcpSocketBase::MpTcpSocketBase(const MpTcpSocketBase& sock) 
  : TcpSocketBase(sock),
    m_peerToken(sock.m_peerToken),
    m_peerKey(sock.m_peerKey),
    m_doChecksum(sock.m_doChecksum),
    m_receivedDSS(sock.m_receivedDSS),
    m_multipleSubflows(sock.m_multipleSubflows),
    m_lastProcessedDack(sock.m_lastProcessedDack),
    m_subflowConnectionSucceeded(sock.m_subflowConnectionSucceeded),
    m_subflowConnectionFailure(sock.m_subflowConnectionFailure),
    m_joinRequest(sock.m_joinRequest),
    m_subflowCreated(sock.m_subflowCreated),
    m_subflowTypeId(sock.m_subflowTypeId),
   m_schedulerTypeId(sock.m_schedulerTypeId)
{
  NS_LOG_FUNCTION(this);
  NS_LOG_LOGIC ("Invoked the copy constructor");
  //! Scheduler may have some states, thus generate a new one
  CreateScheduler(m_schedulerTypeId);
}

MpTcpSocketBase::MpTcpSocketBase()
  : TcpSocketBase(),
    m_peerToken(0),
    m_peerKey(0),
    m_doChecksum(false),
    m_receivedDSS(false),
    m_multipleSubflows(false),
    m_lastProcessedDack(0),
    fLowStartTime(0),
    m_subflowTypeId(MpTcpSubflow::GetTypeId ()),
    m_schedulerTypeId(MpTcpSchedulerRoundRobin::GetTypeId())
{
  NS_LOG_FUNCTION(this);

  //not considered as an Object
  CreateScheduler(m_schedulerTypeId);
  m_subflowConnectionSucceeded  = MakeNullCallback<void, Ptr<MpTcpSubflow> >();
  m_subflowConnectionFailure    = MakeNullCallback<void, Ptr<MpTcpSubflow> >();
}

MpTcpSocketBase::~MpTcpSocketBase(void)
{
  NS_LOG_FUNCTION(this);
  m_node = 0;

  if( m_scheduler )
  {

  }
  m_subflowConnectionSucceeded = MakeNullCallback<void, Ptr<MpTcpSubflow> >();
  m_subflowCreated = MakeNullCallback<void, Ptr<MpTcpSubflow> >();
  m_subflowConnectionSucceeded = MakeNullCallback<void, Ptr<MpTcpSubflow> >();
}

void
MpTcpSocketBase::CreateScheduler(TypeId schedulerTypeId)
{
  NS_LOG_FUNCTION(this);
  NS_LOG_WARN("Overriding scheduler choice");
  ObjectFactory schedulerFactory;
  schedulerTypeId = MpTcpSchedulerRoundRobin::GetTypeId();
  schedulerFactory.SetTypeId(schedulerTypeId);
  m_scheduler = schedulerFactory.Create<MpTcpScheduler>();
  m_scheduler->SetMeta(this);
}

void
MpTcpSocketBase::SetPathManager(PathManagerMode pathManager)
{
  m_pathManager = pathManager;
}

int
MpTcpSocketBase::ConnectNewSubflow(const Address &local, const Address &remote)
{
  NS_ASSERT_MSG(InetSocketAddress::IsMatchingType(local) && InetSocketAddress::IsMatchingType(remote), "only support ipv4");

  NS_LOG_LOGIC("Trying to add a new subflow " << InetSocketAddress::ConvertFrom(local).GetIpv4()
                << "->" << InetSocketAddress::ConvertFrom(remote).GetIpv4());
  // not constructed properly since MpTcpSocketBase creation is hackish
  // and does not call CompleteConstruct
  m_subflowTypeId = MpTcpSubflow::GetTypeId();
  Ptr<Socket> socket = m_tcp->CreateSocket( m_congestionControl, m_subflowTypeId);
  NS_ASSERT(socket);
  Ptr<MpTcpSubflow> sf = DynamicCast<MpTcpSubflow>(socket);
  NS_ASSERT(sf);
  AddSubflow(sf);
  NS_ASSERT(sf->Bind(local) == 0);
  int ret = sf->Connect(remote);

  return ret;
}

uint64_t
MpTcpSocketBase::GetLocalKey() const
{
  return m_mptcpLocalKey;
}

uint32_t
MpTcpSocketBase::GetLocalToken() const
{
  return m_mptcpLocalToken;
}

uint32_t
MpTcpSocketBase::GetPeerToken() const
{
  return m_peerToken;
}

uint64_t
MpTcpSocketBase::GetPeerKey() const
{
  return m_peerKey;
}

void
MpTcpSocketBase::NotifyRemoteAddAddr(Address address)
{
  if (!m_onRemoteAddAddr.IsNull())
   {
     m_onRemoteAddAddr (this, address, 0);
   }
}

bool
MpTcpSocketBase::DoChecksum() const
{
  return false;
}

MpTcpSocketBase::SubflowList::size_type
MpTcpSocketBase::GetNActiveSubflows() const
{
  return m_subflows[Established].size();
}

Ptr<MpTcpSubflow>
MpTcpSocketBase::GetSubflow(uint8_t id) const
{
  NS_ASSERT_MSG(id < m_subflows[Established].size(), "Trying to get an unexisting subflow");
  return m_subflows[Established][id];
}

void
MpTcpSocketBase::SetPeerKey(uint64_t remoteKey)
{
  uint64_t idsn = 0;
  m_peerKey = remoteKey;
  // use the one  from mptcp-crypo.h
  GenerateTokenForKey(HMAC_SHA1, m_peerKey, m_peerToken, idsn);  
}

// in fact it just calls SendPendingData()
int
MpTcpSocketBase::Send(Ptr<Packet> p, uint32_t flags)
{
  // NS_LOG_FUNCTION(this);
  //! This will check for established state
  // OoOキューのサイズを正確に取得
  // NS_LOG_INFO("Out-of-order queue size: " << queueSize);

  return TcpSocketBase::Send(p,flags);
}

bool
MpTcpSocketBase::UpdateWindowSize(const TcpHeader& header)
{
  NS_LOG_FUNCTION(this);
  m_rWnd = header.GetWindowSize();
  return true;
}

/* Inherit from Socket class: Get the max number of bytes an app can read */
uint32_t
MpTcpSocketBase::GetRxAvailable(void) const
{
  NS_LOG_FUNCTION (this);
  return m_rxBuffer->Available();
}

void
MpTcpSocketBase::OnSubflowClosed(Ptr<MpTcpSubflow> subflow, bool reset)
{
  NS_LOG_LOGIC("Subflow " << subflow  << " definitely closed");
  if(reset)
  {
    NS_FATAL_ERROR("Case not handled yet.");
  }
  SubflowList::iterator it = std::remove(m_subflows[Closing].begin(), m_subflows[Closing].end(), subflow);
}

void
MpTcpSocketBase::DumpRxBuffers(Ptr<MpTcpSubflow> sf) const
{
  NS_LOG_INFO("=> Dumping meta RxBuffer ");
  m_rxBuffer->Dump();
  for(int i = 0; i < (int)GetNActiveSubflows(); ++i)
  {
    Ptr<MpTcpSubflow> sf = GetSubflow(i);
    NS_LOG_INFO("=> Rx Buffer of subflow=" << sf);
    sf->m_rxBuffer->Dump();
  }
}

/* May return a bool to let subflow know if it should return a ack ?
   it would leave the possibility for meta to send ack on another subflow.
   We have to extract data from subflows on a per mapping basis because mappings
   may not necessarily be contiguous */
void
MpTcpSocketBase::OnSubflowRecv(Ptr<MpTcpSubflow> sf)
{
  NS_LOG_FUNCTION(this << "Received data from subflow=" << sf);
  NS_LOG_INFO("=> Dumping meta RxBuffer before extraction");
  DumpRxBuffers(sf);
  SequenceNumber32 expectedDSN = m_rxBuffer->NextRxSequence();

  /* Extract one by one mappings from subflow */
  while(true)
  {
    Ptr<Packet> p;
    SequenceNumber64 dsn;
    uint32_t canRead = m_rxBuffer->MaxBufferSize() - m_rxBuffer->Size();

    if(canRead <= 0)
    {
      NS_LOG_LOGIC("No free space in meta Rx Buffer");
      break;
    }
    /* Todo tell if we stop to extract only between mapping boundaries or if Extract */
    p = sf->ExtractAtMostOneMapping(canRead, true, dsn);
    if (p->GetSize() == 0)
    {
      NS_LOG_DEBUG("packet extracted empty.");
      break;
    }
    // THIS MUST WORK. else we removed the data from subflow buffer so it would be lost
    // Pb here, htis will be extracted but will not be saved into the main buffer
    // Notify app to receive if necessary
    if(!m_rxBuffer->Add(p, SEQ64TO32(dsn)))
    {
      NS_FATAL_ERROR("Data might have been lost");
    }
  }
  NS_LOG_INFO("=> Dumping RxBuffers after extraction");
  DumpRxBuffers(sf);
  if (expectedDSN < m_rxBuffer->NextRxSequence())
    {
      NS_LOG_LOGIC("The Rxbuffer advanced");

      // NextRxSeq advanced, we have something to send to the app
      if (!m_shutdownRecv)
        {
          //<< m_receivedData
          NS_LOG_LOGIC("Notify data Rcvd" );
          NotifyDataRecv();
        }
      // Handle exceptions
      if (m_closeNotified)
        {
          NS_LOG_WARN ("Why TCP " << this << " got data after close notification?");
        }
   }
}

void
MpTcpSocketBase::OnSubflowNewCwnd(std::string context, uint32_t oldCwnd, uint32_t newCwnd)
{
  NS_LOG_LOGIC("Subflow updated window from " << oldCwnd << " to " << newCwnd );
  // maybe ComputeTotalCWND should be left
  m_tcb->m_cWnd = ComputeTotalCWND();
}

/* add a MakeBoundCallback that accepts a member function as first input */
static void
onSubflowNewState(
  Ptr<MpTcpSocketBase> meta,
  Ptr<MpTcpSubflow> sf,
  TcpSocket::TcpStates_t  oldState,
  TcpSocket::TcpStates_t newState
  )
{
  NS_LOG_UNCOND("onSubflowNewState wrapper");
    meta->OnSubflowNewState(
      "context", sf, oldState, newState);
}

/* use it to Notify user of, we need a MakeBoundCallback */
void
MpTcpSocketBase::OnSubflowNewState(std::string context,
  Ptr<MpTcpSubflow> sf,
  TcpSocket::TcpStates_t  oldState,
  TcpSocket::TcpStates_t newState
)
{
  NS_LOG_LOGIC("subflow " << sf << " state changed from " << TcpStateName[oldState] << " to " << TcpStateName[newState]);
  NS_LOG_LOGIC("Current rWnd=" << m_rWnd);
  ComputeTotalCWND();

  if(sf->IsMaster() && newState == SYN_RCVD)
  {
    NS_LOG_LOGIC("moving meta to SYN_RCVD");
    m_state = SYN_RCVD;
    m_rWnd = sf->m_rWnd;
  }
  // Only react when it gets established
  if(newState == ESTABLISHED)
  {
    MoveSubflow(sf, Others, Established);

    // subflow did SYN_RCVD -> ESTABLISHED
    if(oldState == SYN_RCVD)
    {
      NS_LOG_LOGIC("Subflow created");
      OnSubflowEstablished(sf);
    }
    else if(oldState == SYN_SENT)
    {
      OnSubflowEstablishment(sf);
    }
    else
    {
      NS_FATAL_ERROR("Unhandled case");
    }
  }
}

void
MpTcpSocketBase::OnSubflowCreated (Ptr<Socket> socket, const Address &from)
{
  NS_LOG_LOGIC(this);
  Ptr<MpTcpSubflow> sf = DynamicCast<MpTcpSubflow>(socket);
  NotifySubflowCreated(sf);
}

void
MpTcpSocketBase::OnSubflowConnectionSuccess (Ptr<Socket> socket)
{
  NS_LOG_LOGIC(this);
  Ptr<MpTcpSubflow> sf = DynamicCast<MpTcpSubflow>(socket);
  NotifySubflowConnected(sf);
}

void
MpTcpSocketBase::OnSubflowConnectionFailure (Ptr<Socket> socket)
{
  NS_LOG_LOGIC(this);
  Ptr<MpTcpSubflow> sf = DynamicCast<MpTcpSubflow>(socket);
  if (sf->IsMaster())
    {
      TcpSocketBase::NotifyConnectionFailed();
    }
  else
    {
      // use a specific callback
      NS_FATAL_ERROR("TODO");
    }
}

void
MpTcpSocketBase::AddSubflow(Ptr<MpTcpSubflow> sflow)
{
  NS_LOG_FUNCTION(sflow);
  Ptr<MpTcpSubflow> sf = sflow;
  bool ok;
  ok = sf->TraceConnect ("CongestionWindow", "CongestionWindow", MakeCallback(&MpTcpSocketBase::OnSubflowNewCwnd, this));
  NS_ASSERT_MSG(ok, "Tracing mandatory to update the MPTCP global congestion window");

  //! We need to act on certain subflow state transitions according to doc "There is not a version with bound arguments."
  ok = sf->TraceConnectWithoutContext ("State", MakeBoundCallback(&onSubflowNewState, this, sf));
  NS_ASSERT_MSG(ok, "Tracing mandatory to update the MPTCP socket state");

  if(sf->IsMaster())
  {
    //! then we update
    m_state = sf->GetState();
    m_mptcpLocalKey = sf->m_mptcpLocalKey;
    m_mptcpLocalToken = sf->m_mptcpLocalToken;
    NS_LOG_DEBUG("Set master key/token to "<< m_mptcpLocalKey << "/" << m_mptcpLocalToken);

    // Those may be overriden later
    m_endPoint = sf->m_endPoint;
    m_endPoint6 = sf->m_endPoint6;
  }
  sf->SetMeta(this);

  /** We override here callbacks so that subflows
   * don't communicate with the applications directly. The meta socket will
   */
  sf->SetConnectCallback (MakeCallback (&MpTcpSocketBase::OnSubflowConnectionSuccess,this),
                          MakeCallback (&MpTcpSocketBase::OnSubflowConnectionFailure,this));   // Ok
  sf->SetAcceptCallback (
                         MakeNullCallback<bool, Ptr<Socket>, const Address &>(),
                         MakeCallback (&MpTcpSocketBase::OnSubflowCreated,this));
  sf->SetCongestionControlAlgorithm(this->m_congestionControl);
  m_subflows[Others].push_back( sf );
}

/* I ended up duplicating this code to update the meta r_Wnd,
   which would have been hackish otherwise */
void
MpTcpSocketBase::ForwardUp (Ptr<Packet> packet, Ipv4Header header, uint16_t port, Ptr<Ipv4Interface> incomingInterface)
{
  NS_LOG_DEBUG(this << " called with endpoint " << m_endPoint);
  NS_FATAL_ERROR("This socket should never receive any packet");
}

bool
MpTcpSocketBase::NotifyJoinRequest (const Address &from, const Address & toAddress)
{
  NS_LOG_FUNCTION (this << &from);
  if (!m_joinRequest.IsNull ())
    {
      return m_joinRequest (this, from, toAddress);
    }
  else
    {
      // accept all incoming connections by default.
      // this way people writing code don't have to do anything
      // special like register a callback that returns true
      // just to get incoming connections
      return true;
    }
}

bool
MpTcpSocketBase::OwnIP(const Address& address) const
{
  NS_LOG_FUNCTION(this);
  NS_ASSERT_MSG(Ipv4Address::IsMatchingType(address), "only ipv4 supported for now");
  Ipv4Address ip = Ipv4Address::ConvertFrom(address);
  Ptr<Node> node = GetNode();
  Ptr<Ipv4L3Protocol> ipv4 = node->GetObject<Ipv4L3Protocol>();
  return (ipv4->GetInterfaceForAddress(ip) >= 0);
  return false;
}

Ipv4EndPoint*
MpTcpSocketBase::NewSubflowRequest(
  Ptr< Packet> p,
  const TcpHeader & tcpHeader,
  const Address & fromAddress,
  const Address & toAddress,
  Ptr<const TcpOptionMpTcpJoin> join
)
{
  NS_LOG_LOGIC("Received request for a new subflow while in state " << TcpStateName[m_state]);
  NS_ASSERT_MSG(InetSocketAddress::IsMatchingType(fromAddress) && InetSocketAddress::IsMatchingType(toAddress),
                "Source and destination addresses should be of the same type");
  NS_ASSERT(join);
  NS_ASSERT(join->GetPeerToken() == GetLocalToken());

  // check we can accept the creation of a new subflow (did we receive a DSS already ?)
  if( !FullyEstablished() )
  {
    NS_LOG_WARN("Received an MP_JOIN while meta not fully established yet.");
    return 0;
  }
  Ipv4Address ip = InetSocketAddress::ConvertFrom(toAddress).GetIpv4();
  if(!OwnIP(ip))
  {
    NS_LOG_WARN("This host does not own the ip " << ip);
    return 0;
  }
  // Similar to NotifyConnectionRequest
  bool accept_connection = NotifyJoinRequest(fromAddress, toAddress);
  if(!accept_connection)
  {
    NS_LOG_LOGIC("Refusing establishement of a new subflow");
    return 0;
  }
  Ptr<MpTcpSubflow> subflow ;
  m_subflowTypeId = MpTcpSubflow::GetTypeId();
  Ptr<Socket> sock = m_tcp->CreateSocket(m_congestionControl, m_subflowTypeId);
  subflow = DynamicCast<MpTcpSubflow>(sock);
  AddSubflow(subflow);
  // Call it now so that endpoint gets allocated
  subflow->CompleteFork(p, tcpHeader, fromAddress, toAddress);
  return subflow->m_endPoint;
}

/* Create a new subflow with different source and destination port pairs */
bool
MpTcpSocketBase::CreateSingleSubflow(uint16_t randomSourcePort, uint16_t randomDestinationPort)
{
  NS_LOG_FUNCTION(this << randomSourcePort << randomDestinationPort);
  Ptr<MpTcpSubflow> subflow;
  Ptr<Socket> sock = m_tcp->CreateSocket(m_congestionControl, MpTcpSubflow::GetTypeId());
  subflow = DynamicCast<MpTcpSubflow>(sock); 
  AddSubflow(subflow);

  // Set up subflow based on different source ports
  Ipv4Address sAddr = m_endPoint->GetLocalAddress();
  uint16_t sPort = randomSourcePort ;
  Ipv4Address dAddr = m_endPoint->GetPeerAddress();
  uint16_t dPort = randomDestinationPort;
  NS_LOG_INFO("Creating Subflow with: sAddr "<< sAddr<<" sPort "<<sPort<< "dAddr "<<dAddr<<" dPort "<<dPort);
  subflow->m_tcb->m_cWnd = m_tcb->m_segmentSize;
  subflow->m_state = SYN_SENT;
  subflow->m_synCount = m_synCount;
  subflow->m_synCount = m_synRetries;
  subflow->m_dataRetrCount = m_dataRetries;
  subflow->m_endPoint = m_tcp->Allocate(GetBoundNetDevice (), sAddr, sPort, dAddr, dPort);
  NS_LOG_INFO ("subflow endPoiont "<< subflow->m_endPoint);
  m_endPoint = subflow->m_endPoint;
  subflow->SetupCallback();
  bool result = m_tcp->AddSocket(this);
  if(!result)
  {
    NS_LOG_INFO("InitiateSinglesubflow/ Can't add socket: already registered ? Can be because of mptcp");
  }
  subflow->SendEmptyPacket(TcpHeader::SYN); 
  return true;
}
 
/* Create a new subflow with different source and destination IP pairs */
bool
MpTcpSocketBase::CreateSubflowsForMesh()
{
  Ipv4Address sAddr = m_endPoint->GetLocalAddress();
  Ipv4Address dAddr = m_endPoint->GetPeerAddress();
  uint16_t sPort    = m_endPoint->GetLocalPort ();
  uint16_t dPort    = m_endPoint->GetPeerPort (); 

  for ( auto it = LocalAddressInfo.begin(); it != LocalAddressInfo.end(); it++ )
    {
      // To get hold of the class pointers:
      Ipv4Address localAddress = it->first;
      for ( auto it = RemoteAddressInfo.begin(); it != RemoteAddressInfo.end(); it++ )
        {
          // To get hold of the class pointers:
          Ipv4Address remoteAddress = it->first;
          uint16_t localPort = rand() % 65000;
          uint16_t remotePort = rand() % 65000;
          if (localPort == sPort)
            {
              NS_LOG_UNCOND("Generated random port is the same as meta subflow's local port, increment by +1");
              localPort++;
            }
          if (remotePort == dPort)
            {
              NS_LOG_UNCOND("Generated random port is the same as meta subflow's peer port, increment by +1");
              remotePort++;
            }
          if ( (sAddr == localAddress) && (dAddr == remoteAddress) )
           {
             continue;
           }
          // Set up subflow based on different source ports
          Ptr<MpTcpSubflow> subflow;
          Ptr<Socket> sock = m_tcp->CreateSocket(m_congestionControl, MpTcpSubflow::GetTypeId());
          subflow = DynamicCast<MpTcpSubflow>(sock); 
          AddSubflow(subflow); 
          subflow->m_tcb->m_cWnd = m_tcb->m_segmentSize;
          subflow->m_state = SYN_SENT;
          subflow->m_synCount = m_synCount;
          subflow->m_synCount = m_synRetries;
          subflow->m_dataRetrCount = m_dataRetries;
          subflow->m_endPoint = m_tcp->Allocate(GetBoundNetDevice (), localAddress, localPort, remoteAddress, remotePort);
          NS_LOG_INFO ("subflow endPoiont "<< subflow->m_endPoint);
          m_endPoint = subflow->m_endPoint;
          subflow->SetupCallback();
          bool result = m_tcp->AddSocket(this);
          if(!result)
          {
            NS_LOG_INFO("InitiateSinglesubflow/ Can't add socket: already registered ? Can be because of mptcp");
          }
          subflow->SendEmptyPacket(TcpHeader::SYN); 
       }
    }
  return true;
}

void
MpTcpSocketBase::SendFastClose(Ptr<MpTcpSubflow> sf)
{
  NS_LOG_LOGIC ("Sending MP_FASTCLOSE");
  TcpHeader header;
  Ptr<TcpOptionMpTcpFastClose> opt = Create<TcpOptionMpTcpFastClose>();

  opt->SetPeerKey( GetPeerKey() );
  sf->SendEmptyPacket(TcpHeader::RST);
  TimeWait();
}

void
MpTcpSocketBase::ConnectionSucceeded(void)
{
  NS_LOG_FUNCTION(this);
  m_connected = true;
  TcpSocketBase::ConnectionSucceeded();
}

bool
MpTcpSocketBase::IsConnected() const
{
  return m_connected;
}

/* Inherited from Socket class: Bind socket to an end-point in MpTcpL4Protocol */
int
MpTcpSocketBase::Bind()
{
  NS_LOG_FUNCTION (this);
  m_endPoint = m_tcp->Allocate();  // Create endPoint with ephemeralPort
  if (0 == m_endPoint)
    {
      m_errno = ERROR_ADDRNOTAVAIL;
      return -1;
    }
  return SetupCallback();
}

/* Clean up after Bind operation. Set up callback function in the end-point */
int
MpTcpSocketBase::SetupCallback()
{
  NS_LOG_FUNCTION(this);
  return TcpSocketBase::SetupCallback();
}

/* Inherit from socket class: Bind socket (with specific address) to an end-point in TcpL4Protocol
cconvert to noop
*/
int
MpTcpSocketBase::Bind(const Address &address)
{
  NS_LOG_FUNCTION (this<<address);
  NS_FATAL_ERROR("TO remove");
  return TcpSocketBase::Bind(address);
}

/*
Notably, it is only DATA_ACKed once all
data has been successfully received at the connection level.  Note,
therefore, that a DATA_FIN is decoupled from a subflow FIN.  It is
only permissible to combine these signals on one subflow if there is
no data outstanding on other subflows.
*/
void
MpTcpSocketBase::PeerClose( SequenceNumber32 dsn, Ptr<MpTcpSubflow> sf)
{
  NS_LOG_LOGIC("Datafin with seq=" << dsn);
  if( dsn < m_rxBuffer->NextRxSequence() || m_rxBuffer->MaxRxSequence() < dsn)
  {
    NS_LOG_INFO("dsn " << dsn << " out of expected range [ " << m_rxBuffer->NextRxSequence()  << " - " << m_rxBuffer->MaxRxSequence() << " ]" );
    return ;
  }
  // For any case, remember the FIN position in rx buffer first
  //! +1 because the datafin doesn't count as payload
  m_rxBuffer->SetFinSequence(dsn);
  NS_LOG_LOGIC ("Accepted MPTCP FIN at seq " << dsn);

  // Return if FIN is out of sequence, otherwise move to CLOSE_WAIT state by DoPeerClose
  if (!m_rxBuffer->Finished())
  {
    NS_LOG_WARN("Out of range");
    return;
  }

  // Simultaneous close: Application invoked Close() when we are processing this FIN packet
  TcpStates_t old_state = m_state;
  switch(m_state)
  {
    case FIN_WAIT_1:
      m_state = CLOSING;
      break;
    case FIN_WAIT_2:
      // will go into timewait later
      TimeWait();
      break;
    case ESTABLISHED:
      m_state = CLOSE_WAIT;
      break;
    default:
      NS_FATAL_ERROR("Should not be here");
      break;
  };
  NS_LOG_INFO(TcpStateName[old_state] << " -> " << TcpStateName[m_state]);
  TcpHeader header;
  sf->AppendDSSAck();
  sf->SendEmptyPacket(TcpHeader::ACK);
}

//This should take care of ReTxTimeout
void
MpTcpSocketBase::NewAck(SequenceNumber32 const& dsn, bool resetRTO)
{
  NS_LOG_FUNCTION(this << " new dataack=[" <<  dsn << "]");
  TcpSocketBase::NewAck(dsn, resetRTO);
}

bool
MpTcpSocketBase::IsInfiniteMappingEnabled() const
{
  return false;
}

// Send 1-byte data to probe for the window size at the receiver when
// the local knowledge tells that the receiver has zero window size
// C.f.: RFC793 p.42, RFC1112 sec.4.2.2.17
void
MpTcpSocketBase::PersistTimeout()
{
  NS_LOG_LOGIC ("PersistTimeout expired at " << Simulator::Now ().GetSeconds ());
  NS_FATAL_ERROR("TODO");
}

void
MpTcpSocketBase::BecomeFullyEstablished()
{
  NS_LOG_FUNCTION (this);
  m_receivedDSS = true;

  // should be called only on client side
  ConnectionSucceeded();
}

bool
MpTcpSocketBase::FullyEstablished() const
{
  NS_LOG_FUNCTION_NOARGS();
  return m_receivedDSS;
}

/* Sending data via subflows with available window size.
   Todo somehow rename to dispatch
   we should not care about IsInfiniteMapping() */
uint32_t
MpTcpSocketBase::SendPendingData(bool withAck)
{
  NS_LOG_FUNCTION(this << "Sending data" << TcpStateName[m_state]);

  // パスマネージャの初期化
  if (FullyEstablished() && !m_multipleSubflows) {
    if (m_pathManager == MpTcpSocketBase::Default) {
      //Default path mananger
    }
    else if (m_pathManager == MpTcpSocketBase::FullMesh) {
      //fullmesh path manager
      Ptr<MpTcpFullMesh> m_fullMesh = Create<MpTcpFullMesh>();
      m_fullMesh->CreateMesh(this);
    }
    else if(m_pathManager == MpTcpSocketBase::nDiffPorts) {
      //ndiffports path manager
      Ptr<MpTcpNdiffPorts> m_ndiffPorts = Create<MpTcpNdiffPorts>();
      uint16_t localport = m_endPoint->GetLocalPort();
      uint16_t remoteport = m_endPoint->GetPeerPort();
      m_ndiffPorts->CreateSubflows(this, localport, remoteport);
    }
    else {
      NS_LOG_WARN("Wrong selection of Path Manager");
    }
    m_multipleSubflows = true;
  }

  // 送信バッファのチェック
  if (m_txBuffer->Size() == 0) {
    NS_LOG_DEBUG("Nothing to send in transmit buffer");
    return 0;
  }

  // 利用可能なウィンドウのチェック
  uint32_t availableWindow = Window();
  uint32_t inFlight = BytesInFlight();
  if (availableWindow <= inFlight) {
    NS_LOG_DEBUG("No available window space - in flight: " << inFlight 
                 << ", window: " << availableWindow);
    return 0;
  }

  uint32_t nbMappingsDispatched = 0;
  SequenceNumber64 dsnHead;
  SequenceNumber32 ssn;
  int subflowArrayId;
  uint16_t length;

  // マッピング生成と送信処理
  while (m_scheduler->GenerateMapping(subflowArrayId, dsnHead, length)) {
    Ptr<MpTcpSubflow> subflow = GetSubflow(subflowArrayId);

    // サブフローのウィンドウチェック
    uint32_t subflowWindow = subflow->Window();
    if (subflowWindow == 0) {
      NS_LOG_DEBUG("Subflow " << subflowArrayId << " has no window space");
      continue;
    }

    // マッピングの追加
    bool ok = subflow->AddLooseMapping(dsnHead, length);
    if (!ok) {
      NS_LOG_WARN("Failed to add mapping for subflow " << subflowArrayId);
      continue;
    }

    // パケットの生成と送信
    SequenceNumber32 dsnTail = SEQ64TO32(dsnHead) + length;
    Ptr<Packet> p = m_txBuffer->CopyFromSequence(length, SEQ64TO32(dsnHead));

    if (p->GetSize() > 0) {
      // パケット送信
      int ret = subflow->Send(p, 0);
      NS_LOG_DEBUG("Send result=" << ret << " for subflow " << subflowArrayId);

      if (ret > 0) {
        nbMappingsDispatched++;

        // シーケンス番号の更新
        NS_ASSERT(dsnHead == SEQ32TO64(m_tcb->m_nextTxSequence));
        SequenceNumber32 nextTxSeq = m_tcb->m_nextTxSequence;

        if (dsnHead <= SEQ32TO64(nextTxSeq) && dsnTail >= nextTxSeq) {
          m_tcb->m_nextTxSequence = dsnTail;
        }

        m_tcb->m_highTxMark = std::max(m_tcb->m_highTxMark.Get(), dsnTail);
        NS_LOG_LOGIC("m_nextTxSequence=" << m_tcb->m_nextTxSequence
                    << " m_highTxMark=" << m_tcb->m_highTxMark);
      }
    }
  }

  // 終了処理
  uint32_t remainingData = m_txBuffer->SizeFromSequence(m_tcb->m_nextTxSequence);
  if (m_closeOnEmpty && (remainingData == 0)) {
    TcpHeader header;
    ClosingOnEmpty(header);
  }

  if (nbMappingsDispatched == 0) {
    NS_LOG_DEBUG("No mappings were dispatched");
  } else {
    NS_LOG_DEBUG("Dispatched " << nbMappingsDispatched << " mappings");
  }

  return nbMappingsDispatched > 0;
}

void
MpTcpSocketBase::OnSubflowDupAck(Ptr<MpTcpSubflow> sf)
{
  NS_LOG_DEBUG("Dup ack signaled by subflow " << sf );
}

/**
Retransmit timeout

This function should be very interesting because one may
adopt different strategies here, like reinjecting on other subflows etc...
Maybe allow for a callback to be set here.
*/
void
MpTcpSocketBase::Retransmit()
{
  NS_LOG_LOGIC(this);
  m_tcb->m_nextTxSequence = m_txBuffer->HeadSequence (); // Start from highest Ack
  m_dupAckCount = 0;
  DoRetransmit(); // Retransmit the packet
}

void
MpTcpSocketBase::DoRetransmit(void)
{
  NS_LOG_FUNCTION (this);

  // 再送対象のサブフローを選択 
  Ptr<MpTcpSubflow> subflow = nullptr;
  Time minRtt = Time::Max();
  
  for(int i = 0; i < (int)GetNActiveSubflows(); i++) {
    Ptr<MpTcpSubflow> sf = GetSubflow(i);
    // TcpSocketBaseから継承したRTT推定値を使用
    Time rtt = sf->m_rtt->GetEstimate();
    if(rtt < minRtt) {
      minRtt = rtt; 
      subflow = sf;
    }
  }

  if(!subflow) {
    NS_LOG_WARN("No suitable subflow for retransmission");
    return;
  }

  // 再送データの準備
  SequenceNumber32 seq = m_txBuffer->HeadSequence();
  uint32_t size = m_txBuffer->SizeFromSequence(seq);

  if(size == 0) {
    NS_LOG_INFO("Nothing to retransmit");
    return;
  }

  // マッピング情報を更新して再送 
  MpTcpMapping mapping;
  mapping.SetHeadDSN(SEQ32TO64(seq));
  mapping.SetMappingSize(size);
  mapping.MapToSSN(subflow->FirstUnmappedSSN());
  
  bool ok = subflow->AddLooseMapping(mapping.HeadDSN(), mapping.GetLength());
  if(!ok) {
    NS_LOG_ERROR("Could not add mapping for retransmission");
    return;
  }

  // 再送データを選択したサブフローで送信
  NS_LOG_INFO("Retransmitting " << size << " bytes on subflow " << subflow);
  subflow->SendPendingData(true);

  // 再送タイマーの更新
  if (!m_retxEvent.IsExpired()) {
    m_retxEvent.Cancel();
  }

  // TcpSocketBaseから継承したm_rtoを使用
  m_retxEvent = Simulator::Schedule(subflow->m_rto.Get(), &MpTcpSocketBase::ReTxTimeout, this);

  // 次の再送タイムアウト値を指数バックオフで計算
  // TracedValueからTimeを取得して比較
  Time currentRto = subflow->m_rto.Get();
  Time maxRto = Seconds(60);
  Time newRto = currentRto * 2;
  
  if (newRto > maxRto) {
    newRto = maxRto;
  }
  subflow->m_rto = newRto;
}

void
MpTcpSocketBase::SendFin()
{
  Ptr<MpTcpSubflow> subflow = GetSubflow(0);
  subflow->AppendDSSFin();
  subflow->SendEmptyPacket(TcpHeader::ACK);
}

void
MpTcpSocketBase::ReTxTimeout()
{
  NS_LOG_FUNCTION(this);
  return TcpSocketBase::ReTxTimeout();
}

// advertise addresses
void
MpTcpSocketBase::AdvertiseAddresses()
{
  NS_LOG_INFO("AdvertiseAvailableAddresses-> ");
  Ptr<Node> node = TcpSocketBase::GetNode ();
  Ptr<Packet> p = Create<Packet> ();
  TcpHeader header;
  SequenceNumber32 s = m_tcb->m_nextTxSequence;
  uint8_t flags = TcpHeader::ACK;
  uint8_t j = 100;

  if (flags & TcpHeader::FIN)
    {
      flags |= TcpHeader::ACK;
    }
  else if (m_state == FIN_WAIT_1 || m_state == LAST_ACK || m_state == CLOSING)
    {
      ++s;
    }
  AddSocketTags (p);
  header.SetFlags (flags);
  header.SetSequenceNumber (s);
  header.SetAckNumber (m_rxBuffer->NextRxSequence ());
  header.SetSourcePort(m_endPoint->GetLocalPort ());
  header.SetDestinationPort(m_endPoint->GetPeerPort ());
  // Object from L3 to access to routing protocol, Interfaces and NetDevices and so on.
  Ptr<Ipv4L3Protocol> ipv4 = node->GetObject<Ipv4L3Protocol>();
  for (uint32_t i = 1; i < ipv4->GetNInterfaces(); i++)
     {
       Ptr<Ipv4Interface> interface = ipv4->GetInterface(i);
       Ipv4InterfaceAddress interfaceAddr = interface->GetAddress(0);
       if (interfaceAddr.GetLocal() == Ipv4Address::GetLoopback())
         continue;
       Address address = InetSocketAddress(interfaceAddr.GetLocal(), m_endPoint->GetLocalPort ());
       Ptr<TcpOptionMpTcpAddAddress> addaddr =  CreateObject<TcpOptionMpTcpAddAddress>();
       uint8_t addrId = j;
       std::cout<<" advertising "<<interfaceAddr.GetLocal()<<" "<<m_endPoint->GetLocalPort ()<<std::endl;
       addaddr->SetAddress (address, addrId);
       NS_LOG_INFO("Appended option" << addaddr);
       header.AppendOption( addaddr );
       j = j + 10 ;
    }
  m_tcp->SendPacket (p, header, m_endPoint->GetLocalAddress (),
                         m_endPoint->GetPeerAddress (), m_boundnetdevice);
}

// add local addresses to the container
void
MpTcpSocketBase::AddLocalAddresses()
{
  NS_LOG_FUNCTION(this);
  Ptr<Node> node = TcpSocketBase::GetNode ();
  Ptr<Ipv4L3Protocol> ipv4 = node->GetObject<Ipv4L3Protocol>();
  for (uint32_t i = 0; i < ipv4->GetNInterfaces(); i++)
    {
      Ptr<Ipv4Interface> interface = ipv4->GetInterface(i);
      Ipv4InterfaceAddress interfaceAddr = interface->GetAddress(0);
      if (interfaceAddr.GetLocal() == Ipv4Address::GetLoopback())
        continue;
      NS_LOG_INFO(" Adding local addresses "<<interfaceAddr.GetLocal()<<" "<<m_endPoint->GetLocalPort ());
      LocalAddressInfo.push_back(std::make_pair(interfaceAddr.GetLocal(), m_endPoint->GetLocalPort ()));
    }
}

void
MpTcpSocketBase::SetNewAddrCallback(Callback<bool, Ptr<Socket>, Address, uint8_t> remoteAddAddrCb,
                          Callback<void, uint8_t> remoteRemAddrCb)
{
  m_onRemoteAddAddr = remoteAddAddrCb;
  m_onAddrDeletion = remoteRemAddrCb;
}

void
MpTcpSocketBase::MoveSubflow(Ptr<MpTcpSubflow> subflow, mptcp_container_t from, mptcp_container_t to)
{
  NS_ASSERT(from != to);
  SubflowList::iterator it = std::find(m_subflows[from].begin(), m_subflows[from].end(), subflow);

  if(it == m_subflows[from].end())
  {
    NS_LOG_ERROR("Could not find subflow in *from* container. It may have already been moved by another callback ");
    DumpSubflows();
    return;
  }
  m_subflows[to].push_back(*it);
  m_subflows[from].erase(it);
}

void
MpTcpSocketBase::DumpSubflows() const
{
  NS_LOG_FUNCTION(this << "\n");
  for(int i = 0; i < Maximum; ++i)
  {

    NS_LOG_UNCOND("===== container [" << containerNames[i] << "]");
    for( SubflowList::const_iterator it = m_subflows[i].begin(); it != m_subflows[i].end(); it++ )
    {
      NS_LOG_UNCOND("- subflow [" << *it  << "]");
    }
  }
}

void
MpTcpSocketBase::MoveSubflow(Ptr<MpTcpSubflow> subflow, mptcp_container_t to)
{
  for(int i = 0; i < Maximum; ++i)
  {
    SubflowList::iterator it = std::find(m_subflows[i].begin(), m_subflows[i].end(), subflow);
    if (it != m_subflows[i].end())
      {
        MoveSubflow(subflow, static_cast<mptcp_container_t>(i), to);
        return;
      }
  }
  NS_FATAL_ERROR("Subflow not found in any container");
}

void
MpTcpSocketBase::OnSubflowEstablished(Ptr<MpTcpSubflow> subflow)
{
  if(subflow->IsMaster())
  {
    NS_LOG_LOGIC("Master subflow created, copying its endpoint");
    m_endPoint = subflow->m_endPoint;
    SetTcp(subflow->m_tcp);
    SetNode(subflow->GetNode());

    if(m_state == SYN_SENT || m_state == SYN_RCVD)
    {
      NS_LOG_LOGIC("Meta " << TcpStateName[m_state] << " -> ESTABLISHED");
      m_state = ESTABLISHED;
    }
    else
    {
      NS_FATAL_ERROR("Unhandled case where subflow got established while meta in " << TcpStateName[m_state] );
    }
    InetSocketAddress addr(subflow->m_endPoint->GetPeerAddress(), subflow->m_endPoint->GetPeerPort());
    NotifyNewConnectionCreated(this, addr);
  }
  ComputeTotalCWND();
}

void
MpTcpSocketBase::OnSubflowEstablishment(Ptr<MpTcpSubflow> subflow)
{
  NS_LOG_LOGIC(this << " (=meta) New subflow " << subflow << " established");
  NS_ASSERT_MSG(subflow, "Contact ns3 team");
  ComputeTotalCWND();

  if(subflow->IsMaster())
  {
    NS_LOG_LOGIC("Master subflow established, moving meta from " << TcpStateName[m_state] << " to ESTABLISHED state");
    if(m_state == SYN_SENT || m_state == SYN_RCVD)
    {
      NS_LOG_LOGIC("Meta " << TcpStateName[m_state] << " -> ESTABLISHED");
      m_state = ESTABLISHED;
    }
    else
    {
      NS_LOG_WARN("Unhandled case where subflow got established while meta in " << TcpStateName[m_state] );
    }

    Simulator::ScheduleNow(&MpTcpSocketBase::ConnectionSucceeded, this);
  }
}

void
MpTcpSocketBase::NotifySubflowCreated(Ptr<MpTcpSubflow> sf)
{
  NS_LOG_FUNCTION(this << sf);
  if (!m_subflowCreated.IsNull ())
  {
    m_subflowCreated (sf);
  }
}

void
MpTcpSocketBase::NotifySubflowConnected(Ptr<MpTcpSubflow> sf)
{
  NS_LOG_FUNCTION(this << sf);
  if (!m_subflowConnectionSucceeded.IsNull ())
    {
      m_subflowConnectionSucceeded (sf);
    }
}

void
MpTcpSocketBase::SetSubflowAcceptCallback(
  Callback<bool, Ptr<MpTcpSocketBase>, const Address &, const Address & > joinRequest,
  Callback<void, Ptr<MpTcpSubflow> > connectionCreated
)
{
  NS_LOG_FUNCTION(this << &joinRequest << " " << &connectionCreated);
  m_joinRequest = joinRequest;
  m_subflowCreated = connectionCreated;
}

void
MpTcpSocketBase::SetSubflowConnectCallback(
  Callback<void, Ptr<MpTcpSubflow> > connectionSucceeded,
  Callback<void, Ptr<MpTcpSubflow> > connectionFailure
  )
{
  NS_LOG_FUNCTION(this << &connectionSucceeded);
  m_subflowConnectionSucceeded = connectionSucceeded;
  m_subflowConnectionFailure = connectionFailure;
}

TypeId
MpTcpSocketBase::GetInstanceTypeId(void) const
{
  return MpTcpSocketBase::GetTypeId();
}

void
MpTcpSocketBase::OnSubflowClosing(Ptr<MpTcpSubflow> sf)
{
  NS_LOG_LOGIC("Subflow has gone into state [");
  MoveSubflow(sf, Established, Closing);
}

void
MpTcpSocketBase::OnSubflowDupack(Ptr<MpTcpSubflow> sf, MpTcpMapping mapping)
{
  NS_LOG_LOGIC("Subflow Dupack TODO.Nothing done by meta");
}

void
MpTcpSocketBase::OnSubflowRetransmit(Ptr<MpTcpSubflow> sf)
{
  NS_LOG_INFO("Subflow retransmit. Nothing done by meta");
}

// if bytes may not really be in flight but rather in subflows buffer
uint32_t
MpTcpSocketBase::BytesInFlight() const
{
  return 0;
  NS_LOG_FUNCTION(this);
  return TcpSocketBase::BytesInFlight();
}

/* This function is added to access m_nextTxSequence in
   MpTcpSchedulerRoundRobin class */
SequenceNumber32
MpTcpSocketBase::Getvalue_nextTxSeq()
{
  return m_tcb->m_nextTxSequence;
}
uint16_t
MpTcpSocketBase::AdvertisedWindowSize(bool scale) const
{
  NS_LOG_FUNCTION (this << scale);
  return TcpSocketBase::AdvertisedWindowSize();
}

uint32_t
MpTcpSocketBase::Window() const
{
  NS_LOG_FUNCTION (this);
  return TcpSocketBase::Window();
}

uint32_t
MpTcpSocketBase::AvailableWindow() const
{
  NS_LOG_FUNCTION (this);
  uint32_t unack = UnAckDataCount(); // Number of outstanding bytes
  uint32_t win = Window(); // Number of bytes allowed to be outstanding
  NS_LOG_LOGIC ("UnAckCount=" << unack << ", Win=" << win);
  return (win < unack) ? 0 : (win - unack);
}

void
MpTcpSocketBase::ClosingOnEmpty(TcpHeader& header)
{
  NS_LOG_INFO("closing on empty called");
}

/* Inherit from Socket class: Kill this socket and signal the peer (if any) */
int
MpTcpSocketBase::Close(void)
{
  NS_LOG_FUNCTION (this);
  return TcpSocketBase::Close();
}

void
MpTcpSocketBase::CloseAllSubflows()
{
  NS_LOG_FUNCTION(this << "Closing all subflows");
  NS_ASSERT( m_state == FIN_WAIT_2 || m_state == CLOSING || m_state == CLOSE_WAIT || m_state == LAST_ACK);

  for(int i = 0; i < Closing; ++i)
  {
    // Established
    NS_LOG_INFO("Closing all subflows in state [" << containerNames [i] << "]");
    for( SubflowList::const_iterator it = m_subflows[i].begin(); it != m_subflows[i].end(); it++ )
    {
      Ptr<MpTcpSubflow> sf = *it;
      NS_LOG_LOGIC("Closing sf " << sf);
      int ret = sf->Close();
      NS_ASSERT_MSG(ret == 0, "Can't close subflow");
    }
    m_subflows[Closing].insert( m_subflows[Closing].end(), m_subflows[i].begin(), m_subflows[i].end());
    m_subflows[i].clear();
  }
}

void MpTcpSocketBase::NotifyDsnGap(SequenceNumber64 expected, SequenceNumber64 received) {
  NS_LOG_WARN("DSN gap detected in subflow " <<
              "Expected=" << expected << " Received=" << received);

  // 他のサブフローでギャップを埋められるか確認
  for (uint32_t i = 0; i < GetNActiveSubflows(); i++) {
    Ptr<MpTcpSubflow> sf = GetSubflow(i);
    if (sf->HasDataInRange(expected, received)) {
      NS_LOG_INFO("Found missing data in subflow " << i);
      // そのサブフローからデータを取得
      sf->ExtractDataInRange(expected, received);
      return;
    }
  }

  // 見つからない場合は再送を要求
  DoRetransmit();
}

void 
MpTcpSocketBase::CheckSubflowsForMissingData(SequenceNumber64 expectedDsn, SequenceNumber64 receivedDsn)
{
  NS_LOG_FUNCTION(this << expectedDsn << receivedDsn);

  // 他のサブフローでギャップを埋められるか確認
  for (uint32_t i = 0; i < GetNActiveSubflows(); i++) {
    Ptr<MpTcpSubflow> sf = GetSubflow(i);
    if (sf->HasDataInRange(expectedDsn, receivedDsn)) {
      NS_LOG_INFO("Found missing data in subflow " << i);
      // そのサブフローからデータを取得して処理
      sf->ExtractDataInRange(expectedDsn, receivedDsn);
      return;
    }
  }

  // 見つからない場合は再送を要求
  NS_LOG_WARN("Missing data not found in any subflow, requesting retransmission");
  DoRetransmit();
}

void MpTcpSocketBase::ReceivedAck(SequenceNumber32 dack, Ptr<MpTcpSubflow> sf, bool count_dupacks) {
  if (!sf || !m_txBuffer) {
    return;
  }

  // バッファ更新
  m_txBuffer->DiscardUpTo(dack);
  m_lastProcessedDack = dack;

  // 複数サブフローがある場合のみ制御 
  if (GetNActiveSubflows() >= 2) {
    // Slow pathからのACK受信時
    if (!sf->IsMaster()) {
      m_dsnState.waitingForSlowPath = false;
      // Fast pathの送信許可
      for (uint32_t i = 0; i < GetNActiveSubflows(); i++) {
        Ptr<MpTcpSubflow> subflow = GetSubflow(i);
        if (subflow->IsMaster()) {
          subflow->SendPendingData(false);
        }
      }
    } else {
      // Fast pathからのACK受信時は待ち状態に
      m_dsnState.waitingForSlowPath = true; 
    }
  }
}

void MpTcpSocketBase::ProcessOutOfOrder(Ptr<Packet> packet, const MpTcpMapping& mapping, Ptr<MpTcpSubflow> subflow) {
  NS_LOG_INFO("ProcessOutOfOrder called");
  
  if (!packet || !subflow) {
    NS_LOG_ERROR("Invalid packet or subflow");
    return;
  }

  SequenceNumber64 currentDsn = mapping.HeadDSN();

  // expectedDsnより前のデータは破棄
  if (currentDsn < m_dsnState.expectedDsn) {
    NS_LOG_INFO("Dropping old data DSN=" << currentDsn.GetValue());
    return;
  }

  // キュー追加前の状態を出力
  std::cout << std::endl;
  std::cout << "=============== New Out-of-order Packet ================";
  std::cout << std::endl;
  std::cout << " DSN=" << mapping.HeadDSN().GetValue() << std::endl;
  std::cout << " Length=" << mapping.GetLength() << std::endl;
  std::cout << " From subflow=" << subflow << std::endl;

  // 古いデータのクリーンアップ
  auto it = m_ofoQueue.begin();
  while (it != m_ofoQueue.end()) {
    if (it->mapping.HeadDSN() < m_dsnState.expectedDsn) {
      it = m_ofoQueue.erase(it); 
    } else {
      ++it;
    }
  }

  // キューに追加して結果を確認
  auto result = m_ofoQueue.insert(OfoQueueItem(packet->Copy(), mapping, subflow));
  if (!result.second) {
    NS_LOG_ERROR("Failed to insert packet into OFO queue");
    return;
  }

  // 更新後のキュー状態を出力
  std::cout << "Queue state (" << m_ofoQueue.size() << " packets):" << std::endl;
  for (const auto& item : m_ofoQueue) {
    std::cout << "- DSN=" << item.mapping.HeadDSN().GetValue()
              << " Length=" << item.mapping.GetLength() 
              << " From=" << item.subflow << std::endl;
  }

  std::cout << "Next expected DSN=" << m_dsnState.expectedDsn.GetValue() << std::endl;
  std::cout << "==================================================" << std::endl;

  // キューの処理を試行
  if (!m_ofoQueue.empty()) {
    NS_LOG_INFO("Attempting to process queued packets...");
    TryProcessOfoQueue();
  }
}

/* Move TCP to Time_Wait state and schedule a transition to Closed state */
void
MpTcpSocketBase::TimeWait()
{
  Time timewait_duration = Seconds(2 * m_msl);
  NS_LOG_INFO (TcpStateName[m_state] << " -> TIME_WAIT "
              << "with duration of " << timewait_duration
              << "; m_msl=" << m_msl);
  CloseAllSubflows();
  m_state = TIME_WAIT;
  CancelAllTimers();
  m_timewaitEvent = Simulator::Schedule(timewait_duration, &MpTcpSocketBase::OnTimeWaitTimeOut, this);
}

void
MpTcpSocketBase::OnTimeWaitTimeOut(void)
{
  // Would normally call CloseAndNotify
  NS_LOG_LOGIC("Timewait timeout expired");
  NS_LOG_UNCOND("after timewait timeout, there are still " << m_subflows[Closing].size() << " subflows pending");

  CloseAndNotify();
}

/* Peacefully close the socket by notifying the upper layer and deallocate end point */
void
MpTcpSocketBase::CloseAndNotify(void)
{
  NS_LOG_FUNCTION (this);
  if (!m_closeNotified)
    {
      NotifyNormalClose();
    }
  if (m_state != TIME_WAIT)
    {
      DeallocateEndPoint();
    }
  m_closeNotified = true;
  NS_LOG_INFO (TcpStateName[m_state] << " -> CLOSED");
  CancelAllTimers();
  m_state = CLOSED;
}

/* Peer sent me a DATA FIN. Remember its sequence in rx buffer.
   It means there won't be any mapping above that dataseq */
void
MpTcpSocketBase::PeerClose(Ptr<Packet> p, const TcpHeader& tcpHeader)
{
  NS_LOG_FUNCTION(this << " PEEER CLOSE CALLED !" << tcpHeader);
  NS_FATAL_ERROR("TO REMOVE. Function overriden by PeerClose(subflow)");
}

/* Received a in-sequence FIN. Close down this socket. 
   FIN is in sequence, notify app and respond with a FIN */
void
MpTcpSocketBase::DoPeerClose(void)
{
  // Move the state to CLOSE_WAIT
  NS_LOG_INFO (TcpStateName[m_state] << " -> CLOSE_WAIT");
  m_state = CLOSE_WAIT;

  if (!m_closeNotified)
    {
      /* The normal behaviour for an application is that, when the peer sent a in-sequence
       * FIN, the app should prepare to close. The app has two choices at this point: either
       * respond with ShutdownSend() call to declare that it has nothing more to send and
       * the socket can be closed immediately; or remember the peer's close request, wait
       * until all its existing data are pushed into the TCP socket, then call Close()
       *  explicitly.
       */
      NS_LOG_LOGIC ("TCP " << this << " calling NotifyNormalClose");
      NotifyNormalClose();
      m_closeNotified = true;
    }
  if (m_shutdownSend)
    { // The application declares that it would not sent any more, close this socket
      Close();
    }
  else
    { // Need to ack, the application will close later
      TcpHeader header;
      Ptr<MpTcpSubflow> sf = GetSubflow(0);
      sf->AppendDSSAck();
      sf->SendEmptyPacket(TcpHeader::ACK);
    }
  if (m_state == LAST_ACK)
  {
    NS_LOG_LOGIC ("TcpSocketBase " << this << " scheduling Last Ack timeout 01 (LATO1)");
    NS_FATAL_ERROR("TODO");
   }
}

/* Do the action to close the socket. Usually send a packet with appropriate
   flags depended on the current m_state.
   use a closeAndNotify in more situations */
int
MpTcpSocketBase::DoClose(void)
{
  NS_LOG_FUNCTION(this << " in state " << TcpStateName[m_state]);

  /*
   * close all subflows
   * send a data fin
   * ideally we should be able to work without any subflows and
   * retransmit as soon as we get a subflow up !
   * we should ask the scheduler on what subflow to send the messages
   */
  TcpHeader header;
  switch (m_state)
  {
  case SYN_RCVD:
  case ESTABLISHED:
     // send FIN to close the peer
     {
       NS_LOG_INFO ("ESTABLISHED -> FIN_WAIT_1");
       m_state = FIN_WAIT_1;
       Ptr<MpTcpSubflow> subflow = GetSubflow(0);
       subflow->AppendDSSFin();
       subflow->SendEmptyPacket(TcpHeader::ACK); 
     }
       break;
  case CLOSE_WAIT:
      {
        // send ACK to close the peer
        NS_LOG_INFO ("CLOSE_WAIT -> LAST_ACK");
        Ptr<MpTcpSubflow> subflow = GetSubflow(0);
        m_state = LAST_ACK;
        subflow->AppendDSSAck();
        subflow->SendEmptyPacket(TcpHeader::ACK);  //changed from header to TcpHeader::ACK
      }
       break;
  case SYN_SENT:
  case CLOSING:
      // Send RST if application closes in SYN_SENT and CLOSING
      NS_LOG_WARN("trying to close while closing..");
      break;
  case LISTEN:
      CloseAndNotify();
      break;
  case LAST_ACK:
      TimeWait();
      break;
  case CLOSED:
  case FIN_WAIT_1:
  case FIN_WAIT_2:
    break;
  case TIME_WAIT:
      break;
  default: /* mute compiler */
      // Do nothing in these four states
      break;
  }
  return 0;
}

Ptr<Packet>
MpTcpSocketBase::Recv(uint32_t maxSize, uint32_t flags)
{
  NS_LOG_FUNCTION(this);
  return TcpSocketBase::Recv(maxSize,flags);
}

uint32_t
MpTcpSocketBase::GetTxAvailable(void) const
{
  NS_LOG_FUNCTION (this);
  uint32_t value = m_txBuffer->Available();
  return value;
}

/* This function should be overridable since it may depend on the CC, cf RFC:
   To compute cwnd_total, it is an easy mistake to sum up cwnd_i across
   all subflows: when a flow is in fast retransmit, its cwnd is
   typically inflated and no longer represents the real congestion
   window.  The correct behavior is to use the ssthresh (slow start
   threshold) value for flows in fast retransmit when computing
   cwnd_total.  To cater to connections that are app limited, the
   computation should consider the minimum between flight_size_i and
   cwnd_i, and flight_size_i and ssthresh_i, where appropriate. */
uint32_t
MpTcpSocketBase::ComputeTotalCWND()
{
  NS_LOG_DEBUG("Cwnd before update=" << Window());
  uint32_t totalCwnd = 0;

  for (uint32_t i = 0; i < Maximum; i++)
  {
    for( SubflowList::const_iterator it = m_subflows[i].begin(); it != m_subflows[i].end(); it++ )
    {
      Ptr<MpTcpSubflow> sf = *it;
       {
         totalCwnd += sf->Window();          // Should be this all the time ?!
       }
    }
  }
  m_tcb->m_cWnd = totalCwnd;
  NS_LOG_DEBUG("Cwnd after computation=" << m_tcb->m_cWnd.Get());
  return totalCwnd;
}

/* Kill this socket. This is a callback function configured to m_endpoint in
   SetupCallback(), invoked when the endpoint is destroyed. */
void
MpTcpSocketBase::Destroy(void)
{
  NS_LOG_FUNCTION(this);
  NS_LOG_INFO("Enter Destroy(" << this << ") m_sockets: )");

  NS_LOG_ERROR("Before unsetting endpoint, check it's not used by subflow ?");
  m_endPoint = 0;
}

}  //namespace ns3
