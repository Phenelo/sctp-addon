'use strict';

const SctpAddon = require('../build/Release/sctp-addon');
const Events = require('events');

exports.createClient = function (opts, cb) {

    const socket = new Events.EventEmitter();

    SctpAddon.client(opts.hosts, opts.remoteHosts, opts.port, opts.initOpts,
        (data) => {
            setImmediate(function() {
                socket.emit('data', data);
            });
        },
        () => {
            socket.emit('disconnect');
        },
        (err) => {
            if (!socket.listeners('error')[0]) {
                cb(err);
            }
            else {
                socket.emit('error', err);
            }
        },
        (err, id) => {

            if (err) {

                return cb(err);
            }

            socket.assocId = id;

            socket.write = function (buffer) {

                SctpAddon.send(buffer);
            };

            socket.disconnect = function (cb) {

                SctpAddon.disconnect(cb);
            };

            return cb(null, socket);
        });
};

exports.debug = function (isOn) {

    let value = 0;

    if (isOn) {
        value = 1;
    }

    SctpAddon.debug(value);
};
