#!/usr/bin/env python
#
#   Copyright (C) 2020 Sean D'Epagnier
#
# This Program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation; either
# version 3 of the License, or (at your option) any later version.  

import multiprocessing, select

class NonBlockingPipeEnd(object):
    def __init__(self, pipe, name, recvfailok):
        self.pipe = pipe
        self.pollin = select.poll()
        self.pollin.register(self.pipe, select.POLLIN)
        self.pollout = select.poll()
        self.pollout.register(self.pipe, select.POLLOUT)
        self.name = name
        self.sendfailcount = 0
        self.failcountmsg = 1
        self.recvfailok = recvfailok

    def fileno(self):
        return self.pipe.fileno()

    def flush(self):
        pass

    def close(self):
        self.pipe.close()

    def recv(self, timeout=0):
#        if self.pipe.poll(timeout):
        if self.pollin.poll(0):
            return self.pipe.recv()
        if not self.recvfailok:
            print('error pipe block on recv!', self.name)
        return False

    def send(self, value):
        if self.pollout.poll(0):
            self.pipe.send(value)
            return True
        
        self.sendfailcount += 1
        if self.sendfailcount == self.failcountmsg:
            print('pipe full (%d)' % self.sendfailcount, self.name, 'cannot send')
            self.failcountmsg *= 10
        return False

def NonBlockingPipe(name, recvfailok=True):
    pipe = multiprocessing.Pipe()
    return NonBlockingPipeEnd(pipe[0], name+'[0]', recvfailok), NonBlockingPipeEnd(pipe[1], name+'[1]', recvfailok)
    
class LineBufferedNonBlockingPipeEnd(NonBlockingPipeEnd):
    def __init__(self, pipe, name):
        super(LineBufferedNonBlockingPipeEnd, self).__init__(pipe, name, True)
            
    def readline(self):
#        if self.pipe.poll():   # pipe poll is really slow!!!!!
        if self.pollin.poll(0):
            return self.pipe.recv()
        return False

    def recv(self):
        return True

# non multiprocessed pipe emulates functions in a simple queue
class NoMPLineBufferedPipeEnd(object):
    def __init__(self, name):
        self.name = name
        self.lines = []

    def fileno(self):
        return 0

    def flush(self):
        pass

    def recv(self, timeout=0):
        return True

    def readline(self):
        if not self.lines:
            return False
        ret = self.lines[0]
        self.lines = self.lines[1:]
        return ret

    def send(self, value):
        if len(self.remote.lines) >= 1000:
            return False
        self.remote.lines.append(value)
        return True
        

def LineBufferedNonBlockingPipe(name, use_multiprocessing):
    if use_multiprocessing:
        pipe = multiprocessing.Pipe()
        return LineBufferedNonBlockingPipeEnd(pipe[0], name+'[0]'), \
            LineBufferedNonBlockingPipeEnd(pipe[1], name+'[1]')

    pipe = NoMPLineBufferedPipeEnd(name+'[0]'), NoMPLineBufferedPipeEnd(name+'[1]')
    pipe[0].remote = pipe[1]
    pipe[1].remote = pipe[0]
    return pipe
