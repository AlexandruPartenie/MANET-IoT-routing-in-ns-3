/*
 * Project 3 - MANET routing in ns-3: AODV vs OLSR vs DSDV under density and mobility.
 * One run = one (protocol, nodes, speed, seed) point. Everything the analysis needs is
 * written as per-packet / per-node CSV into --outDir:
 *   tx.csv       flow,seq,t_tx,src,dst            (every packet handed to the UDP socket)
 *   rx.csv       flow,seq,t_tx,t_rx,hops          (every packet delivered to the sink)
 *   ctrl.csv     node,ctrl_pkts,ctrl_bytes,data_tx (IP-layer transmissions after warm-up)
 *   pos.csv      t,node,x,y                       (snapshots, to report partitioning)
 *   flows.csv    flow,src,dst
 *   summary.csv  run parameters + wall-clock time
 *   flowmon.xml  FlowMonitor output (cross-check)
 *
 * Fairness: placement, waypoints, and source-sink pairs use fixed RNG streams, so all three protocols see exactly the same topology, mobility and traffic.
 * <ns-3>/scratch/ and run:  ./ns3 run "manet-compare --protocol=aodv --nodes=50 --speed=0 --seed=1"
 */

#include "ns3/applications-module.h"
#include "ns3/aodv-module.h"
#include "ns3/core-module.h"
#include "ns3/dsdv-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/olsr-module.h"
#include "ns3/seq-ts-header.h"
#include "ns3/wifi-module.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ManetCompare");

namespace
{
// UDP ports used by the routing daemons in the ns-3 library.
constexpr uint16_t PORT_AODV = 654;
constexpr uint16_t PORT_OLSR = 698;
constexpr uint16_t PORT_DSDV = 269;
constexpr uint16_t PORT_DATA_BASE = 9000;
constexpr uint8_t DEFAULT_TTL = 64;

struct Flow
{
    uint32_t id{0};
    uint32_t src{0};
    uint32_t dst{0};
    Ptr<Socket> tx;
    uint32_t seq{0};
};

std::vector<Flow> g_flows;
std::ofstream g_tx;
std::ofstream g_rx;
std::vector<uint64_t> g_ctrlPkts;
std::vector<uint64_t> g_ctrlBytes;
std::vector<uint64_t> g_dataTx;
double g_warmup = 20.0;
double g_sendStop = 198.0;
double g_interval = 0.5;
uint32_t g_payload = 128;

void
SendPacket(uint32_t f)
{
    if (Simulator::Now().GetSeconds() >= g_sendStop)
    {
        return;
    }
    Flow& fl = g_flows[f];
    SeqTsHeader h; // timestamp = Now() at construction
    h.SetSeq(fl.seq);
    Ptr<Packet> p = Create<Packet>(g_payload - h.GetSerializedSize());
    p->AddHeader(h); // application payload = g_payload bytes in total
    fl.tx->Send(p);
    g_tx << f << ',' << fl.seq << ',' << Simulator::Now().GetSeconds() << ',' << fl.src << ','
         << fl.dst << '\n';
    fl.seq++;
    Simulator::Schedule(Seconds(g_interval), &SendPacket, f);
}

void
RecvPacket(uint32_t f, Ptr<Socket> s)
{
    Ptr<Packet> p;
    Address from;
    while ((p = s->RecvFrom(from)))
    {
        SocketIpTtlTag ttlTag;
        int hops = -1;
        if (p->RemovePacketTag(ttlTag))
        {
            // local delivery sees the TTL after the previous forwarders decremented it
            hops = int(DEFAULT_TTL) - int(ttlTag.GetTtl()) + 1;
        }
        SeqTsHeader h;
        p->RemoveHeader(h);
        g_rx << f << ',' << h.GetSeq() << ',' << h.GetTs().GetSeconds() << ','
             << Simulator::Now().GetSeconds() << ',' << hops << '\n';
    }
}

// Every IPv4 packet leaving a node (originated or forwarded). Classified by UDP dest port.
void
Ipv4Tx(std::string ctx, Ptr<const Packet> pkt, Ptr<Ipv4>, uint32_t)
{
    if (Simulator::Now().GetSeconds() < g_warmup)
    {
        return;
    }
    // ctx = "/NodeList/<id>/$ns3::Ipv4L3Protocol/Tx"
    size_t a = ctx.find("/NodeList/") + 10;
    uint32_t node = std::stoul(ctx.substr(a, ctx.find('/', a) - a));

    Ptr<Packet> c = pkt->Copy();
    Ipv4Header ip;
    c->RemoveHeader(ip);
    if (ip.GetProtocol() != 17)
    {
        return;
    }
    UdpHeader udp;
    c->PeekHeader(udp);
    uint16_t dp = udp.GetDestinationPort();
    if (dp == PORT_AODV || dp == PORT_OLSR || dp == PORT_DSDV)
    {
        g_ctrlPkts[node]++;
        g_ctrlBytes[node] += pkt->GetSize();
    }
    else if (dp >= PORT_DATA_BASE)
    {
        g_dataTx[node]++;
    }
}

void
LogPositions(NodeContainer nodes, std::ofstream* out, double every, double stopT)
{
    double t = Simulator::Now().GetSeconds();
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Vector v = nodes.Get(i)->GetObject<MobilityModel>()->GetPosition();
        *out << t << ',' << i << ',' << v.x << ',' << v.y << '\n';
    }
    if (t + every <= stopT)
    {
        Simulator::Schedule(Seconds(every), &LogPositions, nodes, out, every, stopT);
    }
}
} // namespace

int
main(int argc, char* argv[])
{
    std::string protocol = "aodv";
    std::string outDir = "results/tmp";
    uint32_t nNodes = 50;
    uint32_t seed = 1;
    uint32_t nFlows = 10;
    double speed = 0.0;
    double pause = 2.0;
    double simTime = 200.0;
    double area = 500.0;
    double range = 100.0;
    double rate = 2.0; // packets per second per flow

    //<ns - 3> / scratch / and run:  ./ns3 run "manet-compare --protocol=aodv --nodes=50 --speed=0 --seed=1"
    CommandLine cmd(__FILE__);
    cmd.AddValue("protocol", "aodv | olsr | dsdv", protocol);
    cmd.AddValue("nodes", "number of nodes", nNodes);
    cmd.AddValue("speed", "RandomWaypoint speed [m/s]; 0 = static", speed);
    cmd.AddValue("seed", "RNG run number (independent repetition)", seed);
    cmd.AddValue("outDir", "output directory", outDir);
    cmd.AddValue("simTime", "simulated seconds", simTime);
    cmd.AddValue("warmup", "seconds discarded from the statistics", g_warmup);
    cmd.AddValue("flows", "number of CBR flows", nFlows);
    cmd.AddValue("rate", "packets/s per flow", rate);
    cmd.AddValue("payload", "application payload [bytes]", g_payload);
    cmd.AddValue("area", "side of the square area [m]", area);
    cmd.AddValue("range", "radio range [m]", range);
    cmd.AddValue("pause", "waypoint pause [s]", pause);
    cmd.Parse(argc, argv);

    if (protocol != "aodv" && protocol != "olsr" && protocol != "dsdv")
    {
        NS_FATAL_ERROR("unknown protocol " << protocol);
    }
    auto wallStart = std::chrono::steady_clock::now();
    std::filesystem::create_directories(outDir);

    g_interval = 1.0 / rate;
    g_sendStop = simTime - 2.0; // let in-flight packets drain before the end
    g_ctrlPkts.assign(nNodes, 0);
    g_ctrlBytes.assign(nNodes, 0);
    g_dataTx.assign(nNodes, 0);

    // Same seed number for every protocol -> same random streams below.
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(seed);

    // nodes + mobility
    NodeContainer nodes;
    nodes.Create(nNodes);

    Ptr<UniformRandomVariable> ux = CreateObject<UniformRandomVariable>();
    Ptr<UniformRandomVariable> uy = CreateObject<UniformRandomVariable>();
    ux->SetAttribute("Max", DoubleValue(area));
    uy->SetAttribute("Max", DoubleValue(area));
    ux->SetStream(10);
    uy->SetStream(11);
    Ptr<ListPositionAllocator> initPos = CreateObject<ListPositionAllocator>();
    for (uint32_t i = 0; i < nNodes; ++i)
    {
        initPos->Add(Vector(ux->GetValue(), uy->GetValue(), 0.0));
    }

    MobilityHelper mob;
    mob.SetPositionAllocator(initPos);
    Ptr<PositionAllocator> wpAlloc;
    if (speed <= 0.0)
    {
        mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    }
    else
    {
        ObjectFactory pf;
        pf.SetTypeId("ns3::RandomRectanglePositionAllocator");
        std::string uni = "ns3::UniformRandomVariable[Min=0|Max=" + std::to_string(area) + "]";
        pf.Set("X", StringValue(uni));
        pf.Set("Y", StringValue(uni));
        wpAlloc = pf.Create<PositionAllocator>();
        mob.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                             "Speed",
                             StringValue("ns3::ConstantRandomVariable[Constant=" +
                                         std::to_string(speed) + "]"),
                             "Pause",
                             StringValue("ns3::ConstantRandomVariable[Constant=" +
                                         std::to_string(pause) + "]"),
                             "PositionAllocator",
                             PointerValue(wpAlloc));
    }
    mob.Install(nodes);
    if (speed > 0.0)
    {
        wpAlloc->AssignStreams(20);
        mob.AssignStreams(nodes, 30);
    }

    // Wi-Fi ad hoc
    // ERP-OFDM 6 Mb/s, fixed rate; hard 100 m range RangePropagationLossModel.
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211g);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager","DataMode",StringValue("ErpOfdmRate6Mbps"),"ControlMode",StringValue("ErpOfdmRate6Mbps"));
    YansWifiChannelHelper chan;
    chan.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    chan.AddPropagationLoss("ns3::RangePropagationLossModel", "MaxRange", DoubleValue(range));
    YansWifiPhyHelper phy;
    phy.SetChannel(chan.Create());
    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");
    NetDeviceContainer devs = wifi.Install(phy, mac, nodes);
    // routing + IP
    AodvHelper aodv;
    OlsrHelper olsr;
    DsdvHelper dsdv;
    InternetStackHelper stack;
    if (protocol == "aodv")
    {
        stack.SetRoutingHelper(aodv);
    }
    else if (protocol == "olsr")
    {
        stack.SetRoutingHelper(olsr);
    }
    else
    {
        stack.SetRoutingHelper(dsdv);
    }
    stack.Install(nodes);
    Ipv4AddressHelper addr;
    addr.SetBase("10.1.0.0", "255.255.0.0");
    Ipv4InterfaceContainer ifs = addr.Assign(devs);
    // traffic
    // random source-reciver pairs, fixed per seed
    Ptr<UniformRandomVariable> pick = CreateObject<UniformRandomVariable>();
    pick->SetStream(40);
    Ptr<UniformRandomVariable> startRv = CreateObject<UniformRandomVariable>();
    startRv->SetStream(41);

    std::ofstream flowsCsv(outDir + "/flows.csv");
    flowsCsv << "flow,src,dst\n";
    g_flows.resize(nFlows);
    for (uint32_t f = 0; f < nFlows; ++f)
    {
        uint32_t s = pick->GetInteger(0, nNodes - 1);
        uint32_t d;
        do
        {
            d = pick->GetInteger(0, nNodes - 1);
        } while (d == s);
        g_flows[f].id = f;
        g_flows[f].src = s;
        g_flows[f].dst = d;
        flowsCsv << f << ',' << s << ',' << d << '\n';

        uint16_t port = PORT_DATA_BASE + f;
        Ptr<Socket> rx = Socket::CreateSocket(nodes.Get(d), UdpSocketFactory::GetTypeId());
        rx->Bind(InetSocketAddress(Ipv4Address::GetAny(), port));
        rx->SetIpRecvTtl(true);
        rx->SetRecvCallback(MakeBoundCallback(&RecvPacket, f));

        Ptr<Socket> tx = Socket::CreateSocket(nodes.Get(s), UdpSocketFactory::GetTypeId());
        tx->Connect(InetSocketAddress(ifs.GetAddress(d), port));
        g_flows[f].tx = tx;

        // start in [1,2) s: traffic runs through the warm-up, which is discarded in analysis
        Simulator::Schedule(Seconds(1.0 + startRv->GetValue(0.0, 1.0)), &SendPacket, f);
    }
    flowsCsv.close();

    g_tx.open(outDir + "/tx.csv");
    g_rx.open(outDir + "/rx.csv");
    g_tx << std::setprecision(9) << "flow,seq,t_tx,src,dst\n";
    g_rx << std::setprecision(9) << "flow,seq,t_tx,t_rx,hops\n";




    Config::Connect("/NodeList/*/$ns3::Ipv4L3Protocol/Tx", MakeCallback(&Ipv4Tx));

    std::ofstream posCsv(outDir + "/pos.csv");
    posCsv << "t,node,x,y\n";
    Simulator::Schedule(Seconds(0.0), &LogPositions, nodes, &posCsv, 5.0, simTime);
    FlowMonitorHelper fmh;
    Ptr<FlowMonitor> monitor = fmh.InstallAll();
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    monitor->CheckForLostPackets();
    monitor->SerializeToXmlFile(outDir + "/flowmon.xml", true, true);
    Simulator::Destroy();

    g_tx.close();
    g_rx.close();
    posCsv.close();

    std::ofstream ctrl(outDir + "/ctrl.csv");
    ctrl << "node,ctrl_pkts,ctrl_bytes,data_tx\n";
    for (uint32_t i = 0; i < nNodes; ++i)
    {
        ctrl << i << ',' << g_ctrlPkts[i] << ',' << g_ctrlBytes[i] << ',' << g_dataTx[i] << '\n';
    }
    ctrl.close();

    double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
    std::ofstream sum(outDir + "/summary.csv");
    sum << "protocol,nodes,speed,seed,sim_time,warmup,send_stop,flows,rate_pps,payload_bytes,"
           "wall_s\n";
    sum << protocol << ',' << nNodes << ',' << speed << ',' << seed << ',' << simTime << ','
        << g_warmup << ',' << g_sendStop << ',' << nFlows << ',' << rate << ',' << g_payload << ','
        << wall << '\n';
    sum.close();
    return 0;
}
