/*
  Smart Water Cooler — Backend Server
  Bridges MQTT broker → WebSocket → Browser dashboard

  Setup:
    npm install mqtt ws express

  Run:
    node server.js

  Then open dashboard.html in your browser.
*/

const express = require('express');
const http    = require('http');
const WebSocket = require('ws');
const mqtt    = require('mqtt');
const path    = require('path');

// ─── Config ───────────────────────────────────────────────────────────────────
const HTTP_PORT   = 3000;
const MQTT_BROKER = 'mqtt://YOUR_HIVEMQ_BROKER_URL:1883';  // or mqtts:// for TLS
const MQTT_OPTIONS = {
  username: 'YOUR_MQTT_USERNAME',
  password: 'YOUR_MQTT_PASSWORD',
  clientId: 'Dashboard_Server_' + Math.random().toString(16).slice(2, 8),
};
const MQTT_TOPIC  = 'watercooler/sensors';

// Store last 50 readings in memory
const dataHistory = [];
const MAX_HISTORY = 50;

// ─── Express server (serves dashboard.html) ───────────────────────────────────
const app    = express();
const server = http.createServer(app);
app.use(express.static(path.join(__dirname)));
app.get('/', (req, res) => res.sendFile(path.join(__dirname, 'dashboard.html')));

// ─── WebSocket server ─────────────────────────────────────────────────────────
const wss = new WebSocket.Server({ server });

function broadcast(data) {
  const msg = JSON.stringify(data);
  wss.clients.forEach(client => {
    if (client.readyState === WebSocket.OPEN) client.send(msg);
  });
}

wss.on('connection', (ws) => {
  console.log('Dashboard client connected');
  // Send history on connect so dashboard isn't empty on load
  ws.send(JSON.stringify({ type: 'history', data: dataHistory }));
});

// ─── MQTT client ──────────────────────────────────────────────────────────────
const mqttClient = mqtt.connect(MQTT_BROKER, MQTT_OPTIONS);

mqttClient.on('connect', () => {
  console.log('Connected to MQTT broker');
  mqttClient.subscribe(MQTT_TOPIC, (err) => {
    if (err) console.error('Subscribe error:', err);
    else console.log('Subscribed to topic:', MQTT_TOPIC);
  });
});

mqttClient.on('message', (topic, message) => {
  try {
    const payload = JSON.parse(message.toString());
    payload.received_at = new Date().toISOString();

    // Keep rolling history
    dataHistory.push(payload);
    if (dataHistory.length > MAX_HISTORY) dataHistory.shift();

    // Broadcast to all connected dashboards
    broadcast({ type: 'reading', data: payload });
    console.log('Forwarded reading:', payload);
  } catch (e) {
    console.error('Failed to parse MQTT message:', e.message);
  }
});

mqttClient.on('error', (err) => console.error('MQTT error:', err));

// ─── Start ────────────────────────────────────────────────────────────────────
server.listen(HTTP_PORT, () => {
  console.log(`Server running at http://localhost:${HTTP_PORT}`);
  console.log('Open dashboard.html or visit http://localhost:3000');
});
