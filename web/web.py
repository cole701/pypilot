#!/usr/bin/env python
#
#   Copyright (C) 2019 Sean D'Epagnier
#
# This Program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation; either
# version 3 of the License, or (at your option) any later version.  

from __future__ import print_function
import time, sys, os
from flask import Flask, render_template, session, request, Markup

from flask_socketio import SocketIO, Namespace, emit, join_room, leave_room, \
    close_room, rooms, disconnect
from pypilot.server import LineBufferedNonBlockingSocket
from pypilot import pyjson

pypilot_web_port=80
if len(sys.argv) > 1:
    pypilot_web_port=int(sys.argv[1])
else:
    filename = os.getenv('HOME')+'/.pypilot/web.conf'
    try:
        file = open(filename, 'r')
        config = pyjson.loads(file.readline())
        if 'port' in config:
            pypilot_web_port = config['port']
        file.close()
    except:
        print('using default port of', pypilot_web_port)


# Set this variable to 'threading', 'eventlet' or 'gevent' to test the
# different async modes, or leave it set to None for the application to choose
# the best option based on installed packages.
async_mode = None

app = Flask(__name__)
app.config['SECRET_KEY'] = 'secret!'
socketio = SocketIO(app, async_mode=async_mode)

DEFAULT_PORT = 21311

# determine if we are on tinypilot if piCore is in uname -r
import tempfile, subprocess, os
temp = tempfile.mkstemp()
p=subprocess.Popen(['uname', '-r'], stdout=temp[0], close_fds=True)
p.wait()
f = os.fdopen(temp[0], 'r')
f.seek(0)
kernel_release = f.readline().rstrip()
f.close()
#print('kernel_release', kernel_release)
tinypilot = 'piCore' in kernel_release
#print('tinypilot', tinypilot)
# javascript uses lowercase bool, easier to use int
tinypilot = 1 if tinypilot else 0

@app.route('/')
def index():
    return render_template('index.html', async_mode=socketio.async_mode, pypilot_web_port=pypilot_web_port, tinypilot=tinypilot)

if tinypilot:
    @app.route('/wifi', methods=['GET', 'POST'])
    def wifi():
        networking = '/home/tc/.pypilot/networking.txt'

        wifi = {'mode': 'Master', 'ssid': 'pypilot', 'psk': '', 'client_ssid': 'pypilot', 'client_psk': ''}
        try:
            f = open(networking, 'r')
            while True:
                l = f.readline()
                if not l:
                    break
                try:
                    name, value = l.split('=')
                    wifi[name] = value.rstrip()
                except Exception as e:
                    print('failed to parse line in networking.txt', l)
            f.close()
        except:
            pass

        if request.method == 'POST':
            try:
                for name in request.form:
                    cname = name
                    if request.form['mode'] == 'Managed':
                        cname = 'client_' + name
                    wifi[cname] = str(request.form[name])

                f = open(networking, 'w')
                for name in wifi:
                    f.write(name+'='+wifi[name]+'\n')
                f.close()

                os.system('/opt/networking.sh')
            except Exception as e:
                print('exception!', e)

        return render_template('wifi.html', async_mode=socketio.async_mode, wifi=Markup(wifi))


class MyNamespace(Namespace):
    def __init__(self, name):
        super(Namespace, self).__init__(name)
        socketio.start_background_task(target=self.background_thread)
        self.client = False
        self.polls = {}

    def connect_pypilot(self):
        print('connect pypilot...')
        connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        socketio.emit('flush') # unfortunately needed to awaken socket for client messages
        try:
            connection.connect(('localhost', DEFAULT_PORT))
        except:
            socketio.sleep(2)
            return
        print('connected to pypilot server')

        self.client = pypilotClient()
        self.list_values = self.client.list_values()

    def background_thread(self):
        print('processing clients')
        x = 0
        polls_sent = {}
        while True:
            socketio.sleep(.25)
            sys.stdout.flush() # update log
            polls = {}
            for sid in self.polls:
                for poll in self.polls[sid]:
                    polls[poll] = True
            t = time.time()
            for message in polls:
                if not message in polls_sent or \
                   t - polls_sent[message] > 1:
                    #print('msg', message)
                    self.client.send(message + '\n')
                    polls_sent[message] = t
                    
            self.client.flush()

            events = self.poller.poll(0)
            if not events:
                continue
                
            event = events.pop()
            fd, flag = event
            if flag & (select.POLLHUP | select.POLLERR | select.POLLNVAL) \
               or not self.client.recv():
                print('client disconnected')
                self.client.socket.close()
                socketio.emit('pypilot_disconnect', self.list_values)
                self.client = False
                continue

            while True:
                try:
                    line = self.client.readline()
                    if not line:
                        break
                    #print('line',  line.rstrip())
                    socketio.emit('pypilot', line.rstrip())
                except Exception as e:
                    socketio.emit('log', line)
                    print('error: ', e, line.rstrip())
                    break

    def on_pypilot(self, message):
        if self.client:
            self.client.send(message + '\n')

    def on_pypilot_poll(self, message):
        #print('message', message)
        if message == 'clear':
            self.polls[request.sid] = {}
            return
        self.polls[request.sid][message] = True               

    #def on_disconnect_request(self):
    #    disconnect()

    def on_ping(self):
        emit('pong')

    def on_connect(self):
        #self.clients[request.sid] = Connection()
        #print('Client connected', request.sid, len(self.clients))
        print('Client connected', request.sid)
        self.polls[request.sid] = {}
        socketio.emit('pypilot_connect', self.list_values)

    def on_disconnect(self):
        #client = self.clients[request.sid].client
        #if client:
        #    client.socket.close()
        del self.polls[request.sid]
        if not self.polls:
            if self.client:
                self.client.socket.close()
                self.client = False
                print('closed pypilot client')
        print('Client disconnected', request.sid, len(self.polls))

socketio.on_namespace(MyNamespace(''))

def main():
    import os
    path = os.path.dirname(__file__)
    os.chdir(os.path.abspath(path))
    socketio.run(app, debug=False, host='0.0.0.0', port=pypilot_web_port)

if __name__ == '__main__':
    main()