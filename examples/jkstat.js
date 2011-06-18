var express = require('express');
var kstat = require('kstat');

var app = module.exports = express.createServer();

// Routes

// jkstat getKstat() interface
app.get('/kstat/get/:module/:instance/:name', function(req, res){

        var filter = {};

        filter["module"] = req.params.module;
        filter["name"] = req.params.name;
	filter["instance"] = parseInt(req.params.instance, 10);

        var reader = new kstat.Reader(filter);

	var results = reader.read();

        // Set response header to enable cross-site requests
        res.header('Access-Control-Allow-Origin', '*');

        res.send(results);

});

// Mike Harsch's interface
app.get('/kstat/mget/:module/:instance/:name', function(req, res){

        var filter = {};

        filter["module"] = req.params.module;

        if (req.params.instance ==! '*')
                filter["instance"] = parseInt(req.params.instance, 10);

        //handle multiple semicolon-separated values for stat 'name'
        var stats = req.params.name.split(';');

        var results = {};

        var reader = {};

        for (var i=0; i < stats.length; i++) {
                var stat = stats[i];

                filter["name"] = stat;

                reader[stat] = new kstat.Reader(filter);

                results[stat] = reader[stat].read();
        }
        // Set response header to enable cross-site requests
        res.header('Access-Control-Allow-Origin', '*');

        res.send(results);

});

// jkstat getKstats() interface
app.get('/kstat/list', function(req, res){

        var filter = {};

        var reader = new kstat.Reader(filter);

	var results = reader.list();

        // Set response header to enable cross-site requests
        res.header('Access-Control-Allow-Origin', '*');

        res.send(results);

});

// jkstat chainupdate() interface
app.get('/kstat/chainupdate', function(req, res){

        var filter = {};

        var reader = new kstat.Reader(filter);

	var results = reader.chainupdate();

        // Set response header to enable cross-site requests
        res.header('Access-Control-Allow-Origin', '*');

        res.send(JSON.stringify(results));

});

// jkstat getKCID() interface
app.get('/kstat/getkcid', function(req, res){

        var filter = {};

        var reader = new kstat.Reader(filter);

	var results = reader.getkcid();

        // Set response header to enable cross-site requests
        res.header('Access-Control-Allow-Origin', '*');

        res.send(JSON.stringify(results));

});

// Only listen on $ node app.js

if (!module.parent) {
  app.listen(3000);
  console.log("jkstat server listening on port %d", app.address().port);
}
