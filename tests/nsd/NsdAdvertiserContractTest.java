/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net.mdns;

import android.net.nsd.NsdServiceInfo;
import com.gesher.net.NsdRegistrationContract;
import com.gesher.net.WindowsNsdAdvertiser;
import com.gesher.net.WindowsNsdAdvertiser.*;
import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

/** Genuine core/scheduler with explicit clock/socket boundaries; no Windows execution claim. */
public final class NsdAdvertiserContractTest {
    static int checks;
    static final MdnsFeatureFlags FLAGS = MdnsFeatureFlags.newBuilder().build();
    static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason); checks++;
    }
    static void await(BooleanSupplier condition, String reason) throws Exception {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(4);
        while (!condition.getAsBoolean() && System.nanoTime() < end) Thread.sleep(2);
        check(condition.getAsBoolean(), reason);
    }
    static NsdServiceInfo info(String name) {
        NsdServiceInfo info = new NsdServiceInfo(); info.setServiceName(name);
        info.setServiceType("_gesherreg._tcp"); info.setPort(32411);
        info.setAttribute("binary", new byte[]{0, (byte)255, (byte)128, 1});
        info.setAttribute("none", (byte[])null); info.setAttribute("empty", new byte[0]);
        return info;
    }
    static MdnsPacket parse(byte[] bytes) throws Exception {
        return MdnsPacket.parse(new MdnsPacketReader(bytes, bytes.length, FLAGS));
    }
    static List<MdnsRecord> all(MdnsPacket packet) {
        List<MdnsRecord> result = new ArrayList<>(packet.answers); result.addAll(packet.authorityRecords);
        result.addAll(packet.additionalRecords); return result;
    }
    static MdnsPacket announced(NsdServiceInfo info) throws Exception {
        MdnsRecordRepository repository = new MdnsRecordRepository(new String[]{"Android_OWNED001", "local"}, FLAGS);
        repository.addService(1, info, MdnsAdvertisingOptions.getDefaultOptions());
        return repository.onProbingSucceeded(repository.setServiceProbing(1)).getPacket(0);
    }
    static void layouts(Path out) throws Exception {
        NsdServiceInfo small = info("Owned λ");
        MdnsPacket packet = announced(small);
        byte[] source = MdnsAdvertisingPacket.createRawDnsPacket(new byte[1300], packet);
        byte[][] wire = MdnsWirePackets.responses(packet, 1500, 4);
        check(wire.length == 1 && Arrays.equals(source, wire[0]), "small source writer bytes exact");
        byte[] key = {0,0,3,1,42}; small.setPublicKey(key);
        NsdServiceInfo copied = NsdRegistrationContract.copyForTransport(small);
        small.getAttributes().get("binary")[0] = 44;
        check(copied.getAttributes().get("binary")[0] == 0 && Arrays.equals(copied.getPublicKey(), key),
                "initial transport preserves key and owns opaque bytes");
        check(new NsdServiceInfo(copied).getPublicKey() == null, "later pinned copy omits key");
        MdnsRecordRepository proposedSmall=new MdnsRecordRepository(new String[]{"Android_OWNED001","local"},FLAGS);
        proposedSmall.addService(1,copied,MdnsAdvertisingOptions.getDefaultOptions());
        MdnsPacket proposedPacket=proposedSmall.setServiceProbing(1).getPacket(0);
        byte[][] proposedWire=MdnsWirePackets.probes(proposedPacket,1500,4);
        check(proposedWire.length==1 && Arrays.equals(proposedWire[0],
                MdnsAdvertisingPacket.createRawDnsPacket(new byte[1300],proposedPacket)),"small probe source writer bytes exact");
        Files.write(out.resolve("probe-small.bin"),proposedWire[0]);
        NsdServiceInfo large = new NsdServiceInfo(); large.setServiceName("N".repeat(63));
        large.setServiceType("_" + "t".repeat(62) + "._tcp"); large.setPort(4242);
        for (int i = 0; i < 5; i++) {
            byte[] value = new byte[247]; for (int n=0;n<value.length;n++) value[n]=(byte)(n+i);
            large.setAttribute("p"+i,value);
        }
        byte[] tail = new byte[42]; for (int n=0;n<tail.length;n++) tail[n]=(byte)(255-n);
        large.setAttribute("z",tail); check(large.getTxtRecord().length == 1300, "normal max TXT");
        packet = announced(large);
        for (int family : new int[]{4,6}) {
            int mtu=family==4?1500:1280; wire=MdnsWirePackets.responses(packet,mtu,family);
            List<MdnsRecord> decoded=new ArrayList<>();
            for(int i=0;i<wire.length;i++) {
                MdnsPacket part=parse(wire[i]); decoded.addAll(all(part));
                check(part.flags==0x8400 && part.transactionId==0, "response flags/TC/ID");
                if(wire[i].length>mtu-(family==4?28:48)) check(all(part).size()==1,"oversized single RR only");
                Files.write(out.resolve("large-v"+family+"-"+i+".bin"),wire[i]);
            }
            check(decoded.size()==all(packet).size() && decoded.containsAll(all(packet)),"full binary records in fresh packets");
        }
        List<MdnsRecord> addresses=new ArrayList<>();
        String[] host={"OwnedAddressSet","local"};
        for(int i=0;i<64;i++) {
            byte[] address=new byte[16];address[0]=0x20;address[1]=1;address[2]=0x0d;address[3]=(byte)0xb8;address[15]=(byte)(i+1);
            addresses.add(new MdnsInetAddressRecord(host,0,true,120000,InetAddress.getByAddress(address)));
        }
        MdnsPacket set=new MdnsPacket(0x8400,List.of(),addresses,List.of(),List.of());
        wire=MdnsWirePackets.responses(set,1280,6);int count=0;
        check(wire.length>1,"normal 64-member address set spans datagrams");
        for(int i=0;i<wire.length;i++) {
            Files.write(out.resolve("address-set-v6-"+i+".bin"),wire[i]);
            for(MdnsRecord record:parse(wire[i]).answers) {
                check(record.getCacheFlush(),"every unique member cache flush");count++;
            }
        }
        check(count==64,"complete unique set burst");
        MdnsPointerRecord rr=new MdnsPointerRecord(new String[]{"_own","_tcp","local"},0,false,4500000,
                new String[]{"Instance","_own","_tcp","local"});
        MdnsPointerRecord half=new MdnsPointerRecord(rr.getName(),0,false,2250000,rr.getPointer());
        MdnsPointerRecord more=new MdnsPointerRecord(rr.getName(),0,false,2251000,rr.getPointer());
        check(!MdnsWirePackets.known(rr,List.of(half)),"pinned exact half is not suppressed");
        check(MdnsWirePackets.known(rr,List.of(more)),"pinned greater than half suppressed");
        MdnsPacket completed=MdnsWirePackets.completeReply(List.of(addresses.get(1)),List.of(),List.of(addresses.get(0)),addresses);
        check(completed.answers.size()==64,"partial unique known-answer set restored from authority");
        MdnsRecord txt=packet.answers.stream().filter(r->r instanceof MdnsTextRecord).findFirst().orElseThrow();
        MdnsPacket query=new MdnsPacket(0x5312,0,List.of(new MdnsTextRecord(txt.getName(),false)),List.of(),List.of(),List.of());
        MdnsPacket legacy=parse(MdnsWirePackets.legacy(query,new MdnsPacket(0x8400,List.of(),List.of(txt),List.of(),List.of()))[0]);
        check(legacy.transactionId==0x5312 && legacy.questions.size()==1 && (legacy.flags&0x200)!=0
                && legacy.answers.isEmpty(),"legacy complete question and TC for large mandatory TXT");
        query=new MdnsPacket(0x1234,0,List.of(new MdnsPointerRecord(rr.getName(),false)),List.of(),List.of(),List.of());
        legacy=parse(MdnsWirePackets.legacy(query,new MdnsPacket(0x8400,List.of(),List.of(rr),List.of(),List.of()))[0]);
        check(legacy.transactionId==0x1234 && legacy.questions.size()==1 && (legacy.flags&0x200)==0
                && legacy.answers.get(0).getTtl()==10000 && !legacy.answers.get(0).getCacheFlush(),"legacy framing/TTL");
        List<MdnsRecord> proposed=new ArrayList<>();
        for(MdnsRecord record:addresses) proposed.add(new MdnsInetAddressRecord(host,0,false,120000,
                ((MdnsInetAddressRecord)record).getInet6Address()));
        MdnsPacket probe=new MdnsPacket(0,List.of(new MdnsAnyRecord(host,false)),List.of(),proposed,List.of());
        try {MdnsWirePackets.probes(probe,1280,6);throw new AssertionError("split authority accepted");}
        catch(IOException expected){checks++;}
    }
    static final class Sent {
        final Scope scope; final int family; final InetSocketAddress destination; final byte[] bytes;
        Sent(Scope scope,int family,InetSocketAddress destination,byte[] bytes) {
            this.scope=scope;this.family=family;this.destination=destination;this.bytes=bytes;
        }
    }
    static final class Sim implements Driver {
        volatile long now=1, generation=1;
        volatile boolean closed, failSend;
        volatile Scope[] available;
        private final List<Sent> sent=new ArrayList<>();
        private final java.util.ArrayDeque<Packet> incoming=new java.util.ArrayDeque<>();
        volatile int pollCycles, delivered;
        Sim(boolean hasScope) {available=hasScope?new Scope[]{scope(1)}:new Scope[0];}
        static Scope scope(long generation) {
            Address address=new Address(generation,7,4,new byte[]{(byte)192,0,2,1},24,0,1,1,4,0);
            return new Scope(7,7,9,7001,generation,1,107,1500,0,List.of(address));
        }
        public Scope[] interfaces(){return available;}
        public boolean configure(Scope[] scopes){return true;}
        public long generation(){return generation;}
        public synchronized Packet poll(int timeout)throws IOException {
            pollCycles++;
            if(incoming.isEmpty()) try {wait(Math.max(1,Math.min(timeout,10)));}catch(InterruptedException ignored){}
            Packet packet=incoming.poll();if(packet!=null)delivered++;return packet;
        }
        public synchronized long send(Scope scope,int family,InetSocketAddress destination,byte[][] packets) {
            if(failSend)return 10055L;
            for(byte[] packet:packets)sent.add(new Sent(scope,family,destination,packet.clone()));
            notifyAll();return (long)packets.length<<32;
        }
        public boolean close(){closed=true;return true;}
        void advance(long delta)throws Exception {
            // A visible send/callback can precede the remainder of that owner
            // iteration. Advance only after its repeat deadline is established.
            int cycle=pollCycles;
            await(()->closed||pollCycles>=cycle+2,"owner iteration settled before injected clock advance");
            synchronized(this){now+=delta;notifyAll();}
        }
        synchronized List<Sent> sent(){return new ArrayList<>(sent);}
        synchronized void input(byte[] bytes)throws Exception {
            incoming.add(new Packet(generation,7,4,7,new byte[]{(byte)192,0,2,99},5353,true,255,bytes));notifyAll();
        }
        void observed(Packet packet)throws Exception {
            int cycles, count;
            synchronized(this) {cycles=pollCycles;count=delivered+1;incoming.add(packet);notifyAll();}
            await(()->delivered>=count && pollCycles>=cycles+2,"controlled receive and following owner cycle completed");
        }
    }
    static final class Callbacks implements Callback {
        final List<NsdServiceInfo> registered=Collections.synchronizedList(new ArrayList<>());
        volatile int failures, atFirstSuccess=-1; final Sim sim;
        Callbacks(Sim sim){this.sim=sim;}
        public void registered(NsdServiceInfo info){if(registered.isEmpty())atFirstSuccess=sim.sent().size();registered.add(info);}
        public void failed(int error,boolean retained){failures++;}
    }
    static Owner owner(Sim sim){return new Owner(()->sim,()->sim.now,()->0);}
    static int probes(Sim sim)throws Exception {
        int count=0;for(Sent sent:sim.sent())if(parse(sent.bytes).flags==0)count++;return count;
    }
    static int goodbyes(Sim sim)throws Exception {
        int count=0;for(Sent sent:sim.sent()){
            MdnsPacket packet=parse(sent.bytes);
            if(!packet.answers.isEmpty() && packet.answers.stream().allMatch(r->r.getTtl()==0)) {
                check(packet.additionalRecords.isEmpty() && packet.answers.stream().allMatch(r->r instanceof MdnsPointerRecord
                        && !r.getCacheFlush()),"source goodbye only shared PTR records");count++;
            }
        }return count;
    }
    static void stop(Owner owner,Sim sim,int id)throws Exception {
        CountDownLatch withdrawn=new CountDownLatch(1);owner.unregister(id,withdrawn::countDown);
        check(withdrawn.await(4,TimeUnit.SECONDS),"logical unregister before retirement");
        sim.advance(100);Thread.sleep(20);
        for(int i=0;i<3;i++){sim.advance(2000);Thread.sleep(20);}
        await(()->sim.closed,"real driver close after exit lifetime");
    }
    static void scheduling()throws Exception {
        Sim empty=new Sim(false);Owner owner=owner(empty);Callbacks callbacks=new Callbacks(empty);
        owner.register(1,info("OwnedPending"),MdnsAdvertisingOptions.getDefaultOptions(),12001,callbacks);
        Thread.sleep(20);empty.advance(86400000);Thread.sleep(20);
        check(callbacks.registered.isEmpty() && callbacks.failures==0 && empty.sent().isEmpty(),"empty scopes remain pending without timeout");
        stop(owner,empty,1);
        Sim skip=new Sim(true);owner=owner(skip);callbacks=new Callbacks(skip);
        owner.register(2,info("OwnedSkip"),MdnsAdvertisingOptions.newBuilder().setSkipProbing(true).build(),12001,callbacks);
        final Callbacks skipped=callbacks;
        await(()->skipped.registered.size()==1,"skip logical completion on real scope");
        check(callbacks.atFirstSuccess==0,"skip completion precedes first send");
        await(()->!skip.sent().isEmpty(),"first controlled transport attempt");
        check(probes(skip)==0,"skip has no probe packets");
        stop(owner,skip,2);check(goodbyes(skip)==3,"three source goodbyes");
        Sim normal=new Sim(true);owner=owner(normal);callbacks=new Callbacks(normal);
        owner.register(3,info("OwnedProbe"),MdnsAdvertisingOptions.getDefaultOptions(),12001,callbacks);
        await(()->normal.sent().size()>=1,"initial genuine probe");
        check(callbacks.registered.isEmpty(),"no success before full probe schedule");
        normal.advance(250);await(()->normal.sent().size()>=2,"second probe");
        normal.advance(250);await(()->normal.sent().size()>=3,"third probe");
        check(callbacks.registered.isEmpty(),"final completion interval retained");
        normal.advance(250);final Callbacks normalCb=callbacks;
        await(()->normalCb.registered.size()==1,"success after third completion interval");
        check(probes(normal)==3,"three actual probes");
        stop(owner,normal,3);
        Sim errors=new Sim(true);errors.failSend=true;owner=owner(errors);callbacks=new Callbacks(errors);
        owner.register(4,info("OwnedIoErrors"),MdnsAdvertisingOptions.getDefaultOptions(),12001,callbacks);
        final Owner failedOwner=owner;
        await(()->failedOwner.failedBursts()>=1,"observed first send failure");
        for(int i=2;i<=3;i++){errors.advance(250);final int expected=i;await(()->failedOwner.failedBursts()>=expected,"continued probe after OS send failure");}
        errors.advance(250);final Callbacks failedCb=callbacks;
        await(()->failedCb.registered.size()==1,"logical source success independent of sends");
        check(owner.actualSentDatagrams()==0 && callbacks.failures==0,"no fabricated native sends");
        stop(owner,errors,4);
        CountDownLatch rejected=new CountDownLatch(1);
        new Owner(()->{throw new IOException("controlled construction failure");},()->1,()->0).register(
                5,info("OwnedConstruction"),MdnsAdvertisingOptions.getDefaultOptions(),12001,new Callback(){
                    public void registered(NsdServiceInfo ignored){throw new AssertionError("invented backend");}
                    public void failed(int error,boolean retained){check(error==0&&!retained,"construction failure distinct");rejected.countDown();}
                });
        check(rejected.await(4,TimeUnit.SECONDS),"construction error callback");
    }
    static void replacement()throws Exception {
        Sim sim=new Sim(true);Owner owner=owner(sim);Callbacks old=new Callbacks(sim),fresh=new Callbacks(sim);
        MdnsAdvertisingOptions skip=MdnsAdvertisingOptions.newBuilder().setSkipProbing(true).build();
        owner.register(10,info("OwnedReplacement"),skip,12001,old);
        await(()->!sim.sent().isEmpty(),"old owner announces");
        CountDownLatch gone=new CountDownLatch(1);owner.unregister(10,gone::countDown);
        check(gone.await(4,TimeUnit.SECONDS),"old logical release");
        owner.register(11,info("OwnedReplacement"),skip,12001,fresh);
        await(()->fresh.registered.size()==1,"same name accepted for fresh owner");
        check(fresh.registered.get(0).getServiceName().equals("OwnedReplacement"),"retired name not a live conflict");
        sim.advance(500);Thread.sleep(20);check(goodbyes(sim)==0,"retired goodbye canceled before new owner");
        stop(owner,sim,11);
    }
    public static void main(String[] args)throws Exception {
        try {
            System.load(args[0]);System.load(args[3]);Path out=Path.of(args[1]);Files.createDirectories(out);
            check(android.system.OsConstants.IFA_F_TEMPORARY==1,"genuine selected Java temporary-address flag");
            for(Class<?> type:new Class<?>[]{NsdServiceInfo.class,MdnsRecordRepository.class,MdnsWirePackets.class,WindowsNsdAdvertiser.class}) {
                String origin=type.getProtectionDomain().getCodeSource().getLocation().toString();
                check(Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI()).equals(
                        Path.of(new java.net.URI(args[2]))),"actual candidate provider "+type);System.out.println("PROVIDER "+type.getName()+" "+origin);
            }
            layouts(out);scheduling();replacement();NsdAdvertiserProtocolTest.run(out);NsdAdvertiserRetirementTest.run();
            System.out.println("NSD_ADVERTISER_CONTRACT_OK checks="+checks+" windows=false native_socket=false");
        }catch(Throwable failure){failure.printStackTrace(System.err);throw failure;}
    }
}
