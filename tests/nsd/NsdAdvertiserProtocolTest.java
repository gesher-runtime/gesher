/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net.mdns;

import static com.gesher.net.mdns.NsdAdvertiserContractTest.*;
import android.net.nsd.AdvertisingRequest;
import android.net.nsd.NsdServiceInfo;
import android.net.LinkAddress;
import android.system.OsConstants;
import com.gesher.net.NsdRegistrationContract;
import com.gesher.net.WindowsNsdAdvertiser.*;
import java.net.InetAddress;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Valid owned queries and conflicts through the genuine owner, with explicit I/O/clock seams. */
final class NsdAdvertiserProtocolTest {
    private static final MdnsAdvertisingOptions SKIP =
            MdnsAdvertisingOptions.newBuilder().setSkipProbing(true).build();
    private static byte[] bytes(MdnsPacket packet) throws Exception {
        return MdnsAdvertisingPacket.createRawDnsPacket(new byte[8952], packet);
    }
    private static void settle(Sim sim) throws Exception {
        int cycle=sim.pollCycles;await(()->sim.pollCycles>=cycle+3,"controlled owner cycles completed");
    }
    private static MdnsTextRecord txt(Sim sim) throws Exception {
        for(Sent sent:sim.sent()) for(MdnsRecord record:all(parse(sent.bytes))) {
            if(record instanceof MdnsTextRecord && record.getTtl()>0) return (MdnsTextRecord)record;
        }
        throw new AssertionError("No owned TXT announcement");
    }
    private static MdnsServiceRecord srv(Sim sim) throws Exception {
        for(Sent sent:sim.sent()) for(MdnsRecord record:all(parse(sent.bytes))) {
            if(record instanceof MdnsServiceRecord && record.getTtl()>0) return (MdnsServiceRecord)record;
        }
        throw new AssertionError("No owned SRV proposal/announcement");
    }
    private static void input(Sim sim,int index,int family,String source,int port,MdnsPacket query)throws Exception {
        Scope scope=java.util.Arrays.stream(sim.available).filter(s->s.index==index).findFirst().orElseThrow();
        sim.observed(new Packet(scope.generation,index,family,family==4?scope.ipv4Index:scope.ipv6Index,
                InetAddress.getByName(source).getAddress(),port,true,255,bytes(query)));
    }
    private static MdnsPacket question(MdnsTextRecord record,int id,int flags,boolean qu) {
        return new MdnsPacket(id,flags,List.of(new MdnsTextRecord(record.getName(),qu)),
                List.of(),List.of(),List.of());
    }
    private static Scope ipv6Scope()throws Exception {
        Scope before=Sim.scope(1);
        List<Address> addresses=new ArrayList<>(before.addresses);
        addresses.add(new Address(1,7,6,InetAddress.getByName("2001:db8::1").getAddress(),64,0,1,1,4,0));
        return new Scope(7,7,9,7001,1,3,107,1500,1280,addresses);
    }
    private static Scope secondScope()throws Exception {
        Address address=new Address(1,8,4,InetAddress.getByName("198.51.100.1").getAddress(),24,0,1,1,4,0);
        return new Scope(8,8,10,8001,1,1,108,1500,0,List.of(address));
    }
    private static void knownAnswerGroups()throws Exception {
        // A normal TC train may suppress its own pending answer. A different
        // querier, receiving interface or address family must not alter it.
        for(String mode:List.of("same","other-address","other-port","other-interface","other-family")) {
            System.out.println("PROTOCOL_CASE known-answer-"+mode);
            Sim sim=new Sim(true);
            if(mode.equals("other-interface"))sim.available=new Scope[]{Sim.scope(1),secondScope()};
            if(mode.equals("other-family"))sim.available=new Scope[]{ipv6Scope()};
            Owner owner=owner(sim);Callbacks cb=new Callbacks(sim);
            owner.register(21,info("OwnedTrain-"+mode),SKIP,12001,cb);
            await(()->cb.registered.size()==1 && !sim.sent().isEmpty(),"owned TC service ready: "+mode);settle(sim);
            MdnsTextRecord record=txt(sim);int before=sim.sent().size();
            input(sim,7,4,"192.0.2.99",5353,question(record,0,0x200,true));
            check(sim.sent().size()==before,"TC answer remains deferred");
            int index=mode.equals("other-interface")?8:7;
            int family=mode.equals("other-family")?6:4;
            String source=mode.equals("other-address")?"192.0.2.98"
                    :mode.equals("other-interface")?"198.51.100.99"
                    :mode.equals("other-family")?"2001:db8::99":"192.0.2.99";
            int port=mode.equals("other-port")?42001:5353;
            input(sim,index,family,source,port,new MdnsPacket(0,0,List.of(),List.of(record),List.of(),List.of()));
            sim.advance(500);settle(sim);
            List<Sent> replies=sim.sent().subList(before,sim.sent().size());
            if(mode.equals("same")) check(replies.isEmpty(),"own TC continuation suppresses the known answer");
            else {
                check(replies.size()==1,"distinct legitimate train does not suppress pending answer: "+mode);
                Sent reply=replies.get(0);
                check(reply.scope.index==7 && reply.family==4
                        && reply.destination.getAddress().getHostAddress().equals("192.0.2.99")
                        && parse(reply.bytes).answers.contains(record),"reply remains bound to original querier/scope: "+mode);
            }
            stop(owner,sim,21);
        }
    }
    private static void destinationsAndGeneration(Path out)throws Exception {
        for(String source:List.of("192.0.2.99","198.51.100.99")) {
            Sim sim=new Sim(true);Owner owner=owner(sim);Callbacks cb=new Callbacks(sim);
            owner.register(22,info("OwnedQu"),SKIP,12001,cb);
            await(()->!sim.sent().isEmpty(),"owned QU service ready");settle(sim);
            int before=sim.sent().size();input(sim,7,4,source,5353,question(txt(sim),0,0,true));settle(sim);
            check(sim.sent().size()==before+1,"QU receives one complete response");
            Sent reply=sim.sent().get(before);
            check(reply.destination.getAddress().getHostAddress().equals(
                    source.startsWith("192.")?source:"224.0.0.251"),"actual prefix determines QU versus multicast fallback");
            stop(owner,sim,22);
        }
        Sim sim=new Sim(true);Owner owner=owner(sim);Callbacks cb=new Callbacks(sim);
        owner.register(23,info("OwnedLegacy"),SKIP,12001,cb);
        await(()->!sim.sent().isEmpty(),"owned legacy service ready");settle(sim);
        MdnsTextRecord record=txt(sim);int before=sim.sent().size();
        input(sim,7,4,"192.0.2.99",42002,question(record,0x6001,0,false));
        input(sim,7,4,"192.0.2.99",42002,question(record,0x6002,0,false));settle(sim);
        List<Integer> ids=new ArrayList<>();
        for(int i=before;i<sim.sent().size();i++) {
            Sent reply=sim.sent().get(i);MdnsPacket packet=parse(reply.bytes);ids.add(packet.transactionId);
            check(reply.destination.getPort()==42002 && packet.questions.size()==1
                    && packet.answers.stream().allMatch(r->r.getTtl()<=10000&&!r.getCacheFlush()),
                    "legacy endpoint/TTL/flush boundary");
            Files.write(out.resolve("legacy-"+packet.transactionId+".bin"),reply.bytes);
        }
        check(ids.equals(List.of(0x6001,0x6002)),"same endpoint retains separate legacy transaction IDs");
        before=sim.sent().size();input(sim,7,4,"192.0.2.99",5353,question(record,0,0x200,true));
        Scope old=sim.available[0];
        Address nextAddress=new Address(2,7,4,old.addresses.get(0).bytes,24,0,1,1,4,0);
        sim.available=new Scope[]{new Scope(7,7,9,7002,2,1,107,1500,0,List.of(nextAddress))};
        sim.generation=2;settle(sim);sim.advance(500);settle(sim);
        for(int i=before;i<sim.sent().size();i++) {
            Sent sent=sim.sent().get(i);
            check(sent.scope.generation==2 && sent.scope.luid==7002 && sent.destination.getAddress().isMulticastAddress(),
                    "retired interface generation cannot emit its delayed unicast reply");
        }
        before=sim.sent().size();input(sim,7,4,"192.0.2.99",5353,question(record,0,0,true));settle(sim);
        check(sim.sent().size()==before+1 && sim.sent().get(before).scope.generation==2,
                "same-index returned interface serves new queries under new identity");
        stop(owner,sim,23);
    }
    private static void conflicts()throws Exception {
        for(boolean established:List.of(false,true)) {
            Sim sim=new Sim(true);Owner owner=owner(sim);Callbacks cb=new Callbacks(sim);
            owner.register(24,info("OwnedConflict"),established?SKIP:MdnsAdvertisingOptions.getDefaultOptions(),12001,cb);
            await(()->!sim.sent().isEmpty(),"owned proposal or advertisement ready");settle(sim);
            MdnsServiceRecord before=srv(sim);
            MdnsServiceRecord other=new MdnsServiceRecord(before.getName(),0,true,120000,0,0,
                    before.getServicePort()+1,before.getServiceHost());
            input(sim,7,4,"192.0.2.99",5353,new MdnsPacket(0x8400,List.of(),List.of(other),List.of(),List.of()));
            int probesBefore=probes(sim);sim.advance(4999);settle(sim);
            check(probes(sim)==probesBefore,"conflict retains source 5s retry delay");
            sim.advance(1);await(()->sim.sent().size()>0 && probeCount(sim)>=probesBefore+1,"conflict first reprobe");
            for(int i=2;i<=3;i++){sim.advance(250);final int count=probesBefore+i;await(()->probeCount(sim)>=count,"conflict later reprobe");}
            sim.advance(250);int callbacks=established?2:1;await(()->cb.registered.size()==callbacks,"conflict probe completion callback");
            check(cb.registered.get(callbacks-1).getServiceName().equals(established?"OwnedConflict":"OwnedConflict (2)"),
                    "source conflict rename differs before and after registration");
            stop(owner,sim,24);
        }
    }
    private static int probeCount(Sim sim) {
        try{return probes(sim);}catch(Exception failure){throw new AssertionError(failure);}
    }
    private static void sharedHostAndPort()throws Exception {
        Sim sim=new Sim(true);Owner owner=owner(sim);Callbacks first=new Callbacks(sim),second=new Callbacks(sim);
        owner.register(25,info("OwnedFirst"),SKIP,12001,first);
        owner.register(26,info("OwnedSecond"),SKIP,12001,second);
        await(()->first.registered.size()==1&&second.registered.size()==1,"two live default-host owners");settle(sim);
        MdnsServiceRecord service=srv(sim);int before=sim.sent().size();
        CountDownLatch removed=new CountDownLatch(1);owner.unregister(25,removed::countDown);
        check(removed.await(4,TimeUnit.SECONDS),"first shared-host owner logically retired");
        sim.advance(100);settle(sim);
        input(sim,7,4,"192.0.2.99",5353,new MdnsPacket(0,List.of(new MdnsInetAddressRecord(service.getServiceHost(),MdnsRecord.TYPE_A,true)),List.of(),List.of(),List.of()));
        settle(sim);boolean liveAddress=false;
        for(int i=before;i<sim.sent().size();i++)for(MdnsRecord r:all(parse(sim.sent().get(i).bytes))) {
            if(r instanceof MdnsInetAddressRecord){check(r.getTtl()>0,"retired service does not zero shared host address");liveAddress=true;}
        }
        check(liveAddress&&!sim.closed,"remaining owner retains and answers shared host records");
        stop(owner,sim,26);
        NsdServiceInfo boundary=info("OwnedPortBoundary");boundary.setPort(65536);
        NsdRegistrationContract.checkServiceInfoForRegistration(boundary);
        NsdRegistrationContract.Prepared normalized=NsdRegistrationContract.normalize(new AdvertisingRequest.Builder(boundary).build(),12001);
        check(normalized.service.getPort()==65536,"source admission has no invented positive-port upper bound");
        MdnsPacket wire=parse(bytes(announced(normalized.service)));
        check(wire.answers.stream().filter(r->r instanceof MdnsServiceRecord)
                .allMatch(r->((MdnsServiceRecord)r).getServicePort()==0),"genuine UInt16 writer preserves source port boundary");
    }
    private static void addressAdapter(Path out)throws Exception {
        List<InetAddress> addresses=List.of(InetAddress.getByName("192.0.2.1"),
                InetAddress.getByName("2001:db8::1"),InetAddress.getByName("2001:db8::2"));
        int[] flags={0,OsConstants.IFA_F_DEPRECATED,OsConstants.IFA_F_TEMPORARY};
        List<LinkAddress> links=List.of(new LinkAddress(addresses.get(0),24,flags[0],0),
                new LinkAddress(addresses.get(1),64,flags[1],0),new LinkAddress(addresses.get(2),64,flags[2],0));
        MdnsRecordRepository original=new MdnsRecordRepository(new String[]{"Android_OWNED001","local"},FLAGS);
        MdnsRecordRepository windows=new MdnsRecordRepository(new String[]{"Android_OWNED001","local"},FLAGS);
        original.updateAddresses(links);windows.updateWindowsAddresses(addresses,flags);
        original.addService(1,info("OwnedAddressAdapter"),MdnsAdvertisingOptions.getDefaultOptions());
        windows.addService(1,info("OwnedAddressAdapter"),MdnsAdvertisingOptions.getDefaultOptions());
        byte[] source=bytes(original.onProbingSucceeded(original.setServiceProbing(1)).getPacket(0));
        byte[] adapted=bytes(windows.onProbingSucceeded(windows.setServiceProbing(1)).getPacket(0));
        check(java.util.Arrays.equals(source,adapted),"actual address/family/IFA flags produce exact unchanged upstream wire");
        Files.write(out.resolve("address-adapter-known.bin"),adapted);

        Sim sim=new Sim(true);Scope before=sim.available[0];Address known=before.addresses.get(0);
        Address unknown=new Address(known.generation,known.index,known.family,known.bytes,255,
                known.windowsFlags,known.prefixOrigin,known.suffixOrigin,known.dadState,known.skipAsSource);
        sim.available=new Scope[]{new Scope(before.index,before.ipv4Index,before.ipv6Index,before.luid,
                before.generation,before.families,before.netId,before.mtu4,before.mtu6,List.of(unknown))};
        Owner owner=owner(sim);Callbacks cb=new Callbacks(sim);
        owner.register(27,info("OwnedUnknownPrefix"),SKIP,12001,cb);
        await(()->cb.registered.size()==1&&!sim.sent().isEmpty(),"actual unknown-prefix address still advertises on its scope");settle(sim);
        MdnsPacket announcement=parse(sim.sent().get(0).bytes);
        check(all(announcement).stream().filter(r->r instanceof MdnsInetAddressRecord).anyMatch(
                r->java.util.Arrays.equals(((MdnsInetAddressRecord)r).getInet4Address().getAddress(),known.bytes)),
                "unknown-prefix advertisement preserves actual A bytes");
        Files.write(out.resolve("unknown-prefix-announce-v4.bin"),sim.sent().get(0).bytes);
        int count=sim.sent().size();input(sim,7,4,"192.0.2.99",5353,question(txt(sim),0,0,true));settle(sim);
        check(sim.sent().size()==count+1&&sim.sent().get(count).destination.getAddress().isMulticastAddress()
                &&sim.sent().get(count).scope.addresses.get(0).prefixLength==255,
                "unknown prefix disables QU unicast without inventing or dropping address metadata");
        Files.write(out.resolve("unknown-prefix-qu-v4.bin"),sim.sent().get(count).bytes);
        stop(owner,sim,27);
    }
    static void run(Path out)throws Exception {
        knownAnswerGroups();destinationsAndGeneration(out);conflicts();sharedHostAndPort();addressAdapter(out);
    }
}
