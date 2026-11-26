/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package android.bluetooth;

import com.gesher.server.WindowsBluetooth;
import java.io.IOException;
import java.util.UUID;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

public final class BluetoothSocketHostTest {
    private static void check(boolean ok,String name){if(!ok)throw new AssertionError(name);}
    private static void parallelConnect()throws Exception{
        BluetoothSocket socket=new BluetoothSocket(new BluetoothDevice("00:11:22:33:44:55"),UUID.randomUUID(),1,false);
        AtomicReference<Throwable> failure=new AtomicReference<>();
        Thread first=new Thread(()->{
            try{socket.connect();failure.set(new AssertionError("cancelled connect succeeded"));}
            catch(IOException expected){}catch(Throwable e){failure.set(e);}
        });
        first.start();
        try{
            check(WindowsBluetooth.connecting.await(5,TimeUnit.SECONDS),"first connect entered");
            boolean rejected=false;
            try{socket.connect();}catch(IOException expected){rejected=true;}
            check(rejected,"simultaneous connect is rejected before overwriting the first handle");
            check(WindowsBluetooth.opened.get()==1,"only one native socket opened");
        }finally{socket.close();first.join(5000);}
        check(!first.isAlive() && failure.get()==null,"first connect cancelled cleanly: "+failure.get());
    }
    private static void closedAccept()throws Exception{
        BluetoothServerSocket server=BluetoothServerSocket.listen(null,"fixture",UUID.randomUUID(),false);
        WindowsBluetooth.acceptHook=()->{try{server.close();}catch(IOException e){throw new AssertionError(e);}};
        boolean rejected=false;
        try(BluetoothSocket unexpected=server.accept(5000)){}
        catch(IOException expected){rejected=true;}
        check(rejected,"accept must not publish a socket after listener close");
        check(WindowsBluetooth.acceptedClosed==1,"unpublished accepted socket closed exactly once");
    }
    private static BluetoothSocket connected(){
        return new BluetoothSocket(new BluetoothDevice("00:11:22:33:44:55"),5L,1);
    }
    private static boolean throwsIo(java.io.InputStream in)throws Exception{
        try{in.read();return false;}catch(IOException expected){return true;}
    }
    /** Script: each native read returns the next value; 0 is a peer EOF, negative a native error. */
    private static void script(int... results){
        int[] next={0};
        WindowsBluetooth.readHook=(socket,b,off,len)->{
            int r=results[Math.min(next[0]++,results.length-1)];
            if(r>0)for(int i=0;i<r;i++)b[off+i]=(byte)(0x40+i);
            return r;
        };
    }
    private static void peerEof()throws Exception{
        // Android 17+ (and an app-less runtime, CUR_DEVELOPMENT): clean peer EOF is -1.
        for(int target:new int[]{37,38,10000}){
            BluetoothHost.target=target;
            BluetoothSocket socket=connected();script(2,0);
            byte[] buffer=new byte[8];
            check(socket.getInputStream().read(buffer,1,4)==2&&buffer[1]==0x40&&buffer[2]==0x41,"data before EOF, target "+target);
            check(socket.getInputStream().read(buffer,0,4)==-1,"array read EOF is -1, target "+target);
            check(socket.getInputStream().read()==-1&&socket.getInputStream().read(buffer,0,4)==-1,"EOF is sticky, target "+target);
            check(!socket.isConnected(),"EOF drops the link, target "+target);
            socket.close();
            check(socket.getInputStream().read()==-1,"read after local close is EOF, target "+target);
            check(socket.getInputStream().read(buffer,0,4)==-1,"array read after local close is EOF, target "+target);
            socket=connected();script(0);
            check(socket.getInputStream().read()==-1,"single-byte read EOF is -1, target "+target);
            socket.close();
        }
        // Older targets keep the IOException.
        for(int target:new int[]{36,35,28,1}){
            BluetoothHost.target=target;
            BluetoothSocket socket=connected();script(0);
            check(throwsIo(socket.getInputStream()),"single-byte EOF throws, target "+target);
            socket=connected();script(0);
            boolean threw=false;
            try{socket.getInputStream().read(new byte[4],0,4);}catch(IOException expected){threw=true;}
            check(threw,"array EOF throws, target "+target);
            check(throwsIo(socket.getInputStream()),"read after EOF still throws, target "+target);
            socket.close();
        }
        // Dropped established connections follow the same target gate as clean EOF.
        for(int target:new int[]{36,37,10000})for(int error:new int[]{-10052,-10053,-10054,-10057,-10058,-10060}){
            BluetoothHost.target=target;
            BluetoothSocket socket=connected();script(error);
            if(target>=37){
                check(socket.getInputStream().read(new byte[4],0,4)==-1,"drop returns EOF, target "+target+" error "+error);
                check(socket.getInputStream().read()==-1,"drop EOF is sticky, target "+target);
            }else check(throwsIo(socket.getInputStream()),"legacy drop throws, error "+error);
            check(!socket.isConnected(),"drop retires connected state");socket.close();
        }
        // Native argument/resource failures always throw, whatever the target.
        for(int target:new int[]{36,37,10000}){
            BluetoothHost.target=target;
            BluetoothSocket socket=connected();script(-10022);
            check(throwsIo(socket.getInputStream()),"native error throws, target "+target);
            check(!socket.isConnected(),"native error drops the link, target "+target);
            socket.close();
        }
        // A write can be the first operation to discover a disconnected link.
        for(int target:new int[]{36,37,10000})for(int error:new int[]{-10054,-10022}){
            BluetoothHost.target=target;
            BluetoothSocket socket=connected();
            WindowsBluetooth.writeHook=(handle,b,off,len)->error;
            boolean threw=false;
            try{socket.getOutputStream().write(1);}catch(IOException expected){threw=true;}
            check(threw,"write still throws for disconnect or other error");
            if(target>=37&&error==-10054)check(socket.getInputStream().read()==-1,"write-detected drop makes read EOF");
            else check(throwsIo(socket.getInputStream()),"legacy drop or generic write failure remains IOException");
            socket.close();
        }
        WindowsBluetooth.writeHook=null;
        // Local close interrupts a pending read and follows the target-SDK EOF gate.
        for(int target:new int[]{36,37,10000}){
            BluetoothHost.target=target;
            BluetoothSocket socket=connected();
            WindowsBluetooth.readHook=(handle,b,off,len)->{
                try{socket.close();}catch(IOException e){throw new AssertionError(e);}
                return 0;
            };
            if(target>=37){
                check(socket.getInputStream().read()==-1,"local close wakes read as EOF, target "+target);
                check(socket.getInputStream().read()==-1,"later read on closed socket is EOF, target "+target);
            }else{
                check(throwsIo(socket.getInputStream()),"legacy local close throws");
                check(throwsIo(socket.getInputStream()),"later legacy closed read throws");
            }
        }
        WindowsBluetooth.readHook=null;BluetoothHost.target=10000;
    }
    private static void singleByte()throws Exception{
        BluetoothSocket socket=connected();
        int[] next={0};
        java.util.Set<byte[]> readBuffers=java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<>());
        WindowsBluetooth.readHook=(handle,b,off,len)->{if(len!=1||off!=0)throw new AssertionError("single-byte read len "+len+" off "+off);readBuffers.add(b);b[off]=(byte)next[0]++;return 1;};
        for(int i=0;i<512;i++)check(socket.getInputStream().read()==(i&0xff),"read() returns unsigned byte "+i);
        check(readBuffers.size()==1,"single-byte reads reuse native-boundary storage");
        for(int i=0;i<256;i++)socket.getOutputStream().write(i+(i%2==0?0:0x100*3));
        check(WindowsBluetooth.writeBuffers.size()==1,"single-byte writes reuse native-boundary storage");
        byte[] out;synchronized(WindowsBluetooth.written){out=WindowsBluetooth.written.toByteArray();}
        check(out.length==256,"one native byte per write(int)");
        for(int i=0;i<256;i++)check((out[i]&0xff)==i,"write(int) sends the low 8 bits of "+i);
        // Concurrent single-byte users must not corrupt each other's byte.
        int threads=4,per=2000;
        java.util.concurrent.atomic.AtomicInteger bad=new java.util.concurrent.atomic.AtomicInteger();
        java.util.concurrent.CountDownLatch go=new java.util.concurrent.CountDownLatch(1);
        ThreadLocal<Integer> readValue=new ThreadLocal<>();
        WindowsBluetooth.readHook=(handle,b,off,len)->{b[off]=(byte)(int)readValue.get();Thread.yield();return 1;};
        synchronized(WindowsBluetooth.written){WindowsBluetooth.written.reset();}
        Thread[] all=new Thread[threads];
        for(int t=0;t<threads;t++){
            final int value=0x11*(t+1);
            all[t]=new Thread(()->{
                try{readValue.set(value);go.await();
                    for(int i=0;i<per;i++){
                        if(socket.getInputStream().read()!=value)bad.incrementAndGet();
                        socket.getOutputStream().write(value);
                    }
                }catch(Exception e){bad.incrementAndGet();}
            });all[t].start();
        }
        go.countDown();for(Thread t:all)t.join();
        check(bad.get()==0,"concurrent single-byte reads corrupted");
        synchronized(WindowsBluetooth.written){
            byte[] mixed=WindowsBluetooth.written.toByteArray();
            check(mixed.length==threads*per,"all concurrent single-byte writes sent");
            int[] counts=new int[256];for(byte v:mixed)counts[v&0xff]++;
            for(int t=0;t<threads;t++)check(counts[0x11*(t+1)]==per,"every write(int) value sent exactly once: "+t);
        }
        WindowsBluetooth.readHook=null;socket.close();
    }
    private static void blockedSingleByteClose()throws Exception{
        BluetoothHost.target=37;
        BluetoothSocket socket=connected();
        java.util.concurrent.CountDownLatch entered=new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.CountDownLatch released=new java.util.concurrent.CountDownLatch(1);
        AtomicReference<Throwable> failure=new AtomicReference<>();
        WindowsBluetooth.closeHook=released::countDown;
        WindowsBluetooth.readHook=(handle,b,off,len)->{
            entered.countDown();
            try{check(released.await(5,TimeUnit.SECONDS),"close reaches the blocked native read");}
            catch(InterruptedException e){throw new AssertionError(e);}
            return 0;
        };
        Thread reader=new Thread(()->{
            try{check(socket.getInputStream().read()==-1,"locally closed modern read returns EOF");}
            catch(Throwable e){failure.set(e);}
        });
        reader.start();
        try{
            check(entered.await(5,TimeUnit.SECONDS),"single-byte native read entered");
            socket.close();
            reader.join(5000);
            check(!reader.isAlive()&&failure.get()==null,"close interrupts buffered single-byte read: "+failure.get());
        }finally{
            released.countDown();reader.join(5000);
            WindowsBluetooth.readHook=null;WindowsBluetooth.closeHook=null;socket.close();BluetoothHost.target=10000;
        }
    }
    private static void reconnectAfterEof()throws Exception{
        BluetoothHost.target=37;
        WindowsBluetooth.opened.set(10); // the fixture's handle 1 is the blocked-connect case
        BluetoothSocket socket=connected();
        java.util.concurrent.CountDownLatch entered=new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.CountDownLatch writeEntered=new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.CountDownLatch released=new java.util.concurrent.CountDownLatch(1);
        AtomicReference<Throwable> failure=new AtomicReference<>();
        java.util.concurrent.atomic.AtomicInteger oldReads=new java.util.concurrent.atomic.AtomicInteger();
        java.util.concurrent.atomic.AtomicInteger retired=new java.util.concurrent.atomic.AtomicInteger();
        WindowsBluetooth.closeHook=retired::incrementAndGet;
        WindowsBluetooth.writeHook=(handle,b,off,len)->{
            writeEntered.countDown();
            try{check(released.await(5,TimeUnit.SECONDS),"old write completes after reconnect");}
            catch(InterruptedException e){throw new AssertionError(e);}
            return -10054;
        };
        WindowsBluetooth.readHook=(handle,b,off,len)->{
            if(handle!=5){b[off]=0x55;return 1;}
            if(oldReads.getAndIncrement()==0){
                entered.countDown();
                try{check(released.await(5,TimeUnit.SECONDS),"old read completes after reconnect");}
                catch(InterruptedException e){throw new AssertionError(e);}
            }
            return 0;
        };
        Thread oldReader=new Thread(()->{
            try{check(socket.getInputStream().read(new byte[1],0,1)==-1,"old connection reports its EOF");}
            catch(Throwable e){failure.set(e);}
        });
        Thread oldWriter=new Thread(()->{
            try{socket.getOutputStream().write(new byte[1]);failure.set(new AssertionError("old write should fail"));}
            catch(IOException expected){}catch(Throwable e){failure.set(e);}
        });
        oldReader.start();oldWriter.start();
        try{
            check(entered.await(5,TimeUnit.SECONDS),"old connection has an outstanding read");
            check(writeEntered.await(5,TimeUnit.SECONDS),"old connection has an outstanding write");
            check(socket.getInputStream().read()==-1,"second old read observes EOF");
            socket.connect();
            check(retired.get()==1,"reconnect retires exactly the old native registry entry");
            check(socket.isConnected()&&socket.getInputStream().read()==0x55,"fresh connection clears sticky EOF");
            released.countDown();oldReader.join(5000);oldWriter.join(5000);
            check(!oldReader.isAlive()&&!oldWriter.isAlive()&&failure.get()==null,"late old operations complete: "+failure.get());
            check(socket.isConnected()&&socket.getInputStream().read()==0x55,"late old EOF preserves fresh connection");
        }finally{
            released.countDown();oldReader.join(5000);oldWriter.join(5000);socket.close();
            WindowsBluetooth.readHook=null;WindowsBluetooth.writeHook=null;WindowsBluetooth.closeHook=null;BluetoothHost.target=10000;
        }
    }
    public static void main(String[] args)throws Exception{
        switch(args[0]){
            case "parallel-connect":parallelConnect();break;
            case "closed-accept":closedAccept();break;
            case "peer-eof":peerEof();break;
            case "single-byte":singleByte();break;
            case "blocked-single-byte-close":blockedSingleByteClose();break;
            case "reconnect-after-eof":reconnectAfterEof();break;
            default:throw new AssertionError("unknown case "+args[0]);
        }
        System.out.println("BluetoothSocketHostTest passed: "+args[0]);
    }
}
