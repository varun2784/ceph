// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef __SIMPLEMESSENGER_H
#define __SIMPLEMESSENGER_H


#include <list>
#include <map>
using namespace std;
#include <ext/hash_map>
#include <ext/hash_set>
using namespace __gnu_cxx;


#include "include/types.h"

#include "common/Mutex.h"
#include "common/Cond.h"
#include "common/Thread.h"

#include "Messenger.h"
#include "Message.h"
#include "tcp.h"



/* Rank - per-process
 */
class Rank {
public:
  void sigint();

private:
  class EntityMessenger;
  class Pipe;

  // incoming
  class Accepter : public Thread {
  public:
    bool done;

    int       listen_sd;
    
    Accepter() : done(false) {}
    
    void *entry();
    void stop();
    int start();
  } accepter;

  void sigint(int r);
  

  // pipe
  class Pipe {
  public:
    enum {
      STATE_ACCEPTING,
      STATE_CONNECTING,
      STATE_OPEN,
      STATE_CLOSED,
      STATE_CLOSING
      //STATE_GOTCLOSE,  // got (but haven't sent) a close
      //STATE_SENTCLOSE  // sent (but haven't got) a close
    };

    int sd;
    int new_sd;
    entity_addr_t peer_addr;

    Mutex lock;
    int state;

  protected:

    utime_t first_fault;   // time of original failure
    utime_t last_attempt;  // time of last reconnect attempt

    bool reader_running;
    bool writer_running;

    list<Message*> q;
    list<Message*> sent;
    Cond cond;
    
    __u32 connect_seq;
    __u32 out_seq;
    __u32 in_seq, in_seq_acked;
    
    int accept();   // server handshake
    int connect();  // client handshake
    void reader();
    void writer();

    Message *read_message();
    int write_message(Message *m);
    int do_sendmsg(int sd, struct msghdr *msg, int len);
    int write_ack(unsigned s);

    void fault(bool silent=false);
    void fail();

    void take_queue(list<Message*>& ls) {
      ls.splice(ls.begin(), q);
      ls.splice(ls.begin(), sent);
    }

    // threads
    class Reader : public Thread {
      Pipe *pipe;
    public:
      Reader(Pipe *p) : pipe(p) {}
      void *entry() { pipe->reader(); return 0; }
    } reader_thread;
    friend class Reader;

    class Writer : public Thread {
      Pipe *pipe;
    public:
      Writer(Pipe *p) : pipe(p) {}
      void *entry() { pipe->writer(); return 0; }
    } writer_thread;
    friend class Writer;
    
  public:
    Pipe(int st) : 
      sd(0),
      state(st), 
      reader_running(false), writer_running(false),
      connect_seq(0),
      out_seq(0), in_seq(0), in_seq_acked(0),
      reader_thread(this), writer_thread(this) { }

    void start_reader() {
      reader_running = true;
      reader_thread.create();
    }
    void start_writer() {
      writer_running = true;
      writer_thread.create();
    }

    // public constructors
    static const Pipe& Server(int s);
    static const Pipe& Client(const entity_addr_t& pi);

    entity_addr_t& get_peer_addr() { return peer_addr; }

    void register_pipe();
    void unregister_pipe();
    void dirty_close();
    void join() {
      if (writer_thread.is_started()) writer_thread.join();
      if (reader_thread.is_started()) {
	reader_thread.kill(SIGUSR1);
	reader_thread.join();
      }
    }

    void send(Message *m) {
      lock.Lock();
      _send(m);
      lock.Unlock();
    }    
    void _send(Message *m) {
      q.push_back(m);
      m->set_seq(++out_seq);
      cond.Signal();
    }

    void force_close() {
      ::close(sd);
    }
  };


  // messenger interface
  class EntityMessenger : public Messenger {
    Mutex lock;
    Cond cond;
    list<Message*> dispatch_queue;
    list<Message*> prio_dispatch_queue;
    bool stop;
    int qlen, pqlen;
    int my_rank;

    class DispatchThread : public Thread {
      EntityMessenger *m;
    public:
      DispatchThread(EntityMessenger *_m) : m(_m) {}
      void *entry() {
        m->dispatch_entry();
        return 0;
      }
    } dispatch_thread;
    void dispatch_entry();

    friend class Rank;

  public:
    void queue_message(Message *m) {
      // set recv stamp
      m->set_recv_stamp(g_clock.now());
      
      lock.Lock();
      if (m->get_source().is_mon()) {
	prio_dispatch_queue.push_back(m);
	pqlen++;
      } else {
	qlen++;
	dispatch_queue.push_back(m);
      }
      cond.Signal();
      lock.Unlock();
    }

  public:
      EntityMessenger(entity_name_t name, int r) : 
      Messenger(name),
      stop(false),
      qlen(0), pqlen(0),
      my_rank(r),
      dispatch_thread(this) { }
    ~EntityMessenger() {
      // join dispatch thread
      if (dispatch_thread.is_started())
	dispatch_thread.join();
    }

    void ready();
    bool is_stopped() { return stop; }

    void wait() {
      dispatch_thread.join();
    }
    
    int get_dispatch_queue_len() { return qlen + pqlen; }

    void reset_myname(entity_name_t m);

    int shutdown();
    void suicide();
    void prepare_dest(const entity_addr_t& addr);
    int send_message(Message *m, entity_inst_t dest);
    
    void mark_down(entity_addr_t a);
    void mark_up(entity_name_t a, entity_addr_t& i);
  };


  // Rank stuff
 public:
  Mutex lock;
  Cond  wait_cond;  // for wait()
  bool started;

  // where i listen
  entity_addr_t rank_addr;
  
  // local
  unsigned max_local, num_local;
  vector<EntityMessenger*> local;
  vector<bool>             stopped;
  
  // remote
  hash_map<entity_addr_t, Pipe*> rank_pipe;

  set<Pipe*>      pipes;
  list<Pipe*>     pipe_reap_queue;
        
  Pipe *connect_rank(const entity_addr_t& addr);

  const entity_addr_t &get_rank_addr() { return rank_addr; }

  void mark_down(entity_addr_t addr);

  void reaper();

public:
  Rank() : started(false),
	   max_local(0), num_local(0) { }
  ~Rank() { }

  //void set_listen_addr(tcpaddr_t& a);

  int start_rank();
  void wait();

  EntityMessenger *register_entity(entity_name_t addr);
  void rename_entity(EntityMessenger *ms, entity_name_t newaddr);
  void unregister_entity(EntityMessenger *ms);

  void submit_message(Message *m, const entity_addr_t& addr);  
  void prepare_dest(const entity_addr_t& addr);

  // create a new messenger
  EntityMessenger *new_entity(entity_name_t addr);

} ;



extern Rank rank;

#endif
