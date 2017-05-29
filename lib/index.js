'use strict';

const SctpAddon = require('../build/Release/sctp-addon');
const Events = require('events');

exports.createClient = function (opts, cb) {

    const rawSocket = new Events.EventEmitter();

    SctpAddon.connect(rawSocket, opts.hosts, opts.remoteHosts, opts.port, opts.initOpts, (err, socket) => {

        socket.write = function (buffer) {
            SctpAddon.send(buffer);
        };

        return cb(err, socket);
    });
};

exports.debug = function (isOn) {

    let value = 0;

    if (isOn) {
        value = 1;
    }

    SctpAddon.debug(value);
};
