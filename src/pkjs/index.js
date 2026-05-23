var Clay = require('pebble-clay');

var clayConfig = [
  { "type": "heading", "defaultValue": "Gemini Configuration" },
  {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "Personalization" },
      {
        "type": "input", 
        "messageKey": "CUSTOM_PERSONA",
        "label": "Custom Assistant Memory",
        "description": "Tell Gemini who you are or how it should behave (e.g., 'My name is Eric. I am an engineer. Keep answers brief.').",
        "attributes": {
          "spellcheck": "true"
        }
      }
    ]
  },
  {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "API Keys" },
      {
        "type": "input",
        "messageKey": "GEMINI_API_KEY",
        "label": "Gemini API Key",
        "attributes": {
          "type": "text",
          "autocorrect": "off",
          "autocapitalize": "off"
        }
      },
      {
        "type": "input",
        "messageKey": "TTS_API_KEY",
        "label": "Google Cloud TTS API Key",
        "attributes": {
          "type": "text",
          "autocorrect": "off",
          "autocapitalize": "off"
        }
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "heading",
        "defaultValue": "Display Settings"
      },
      {
        "type": "slider",
        "messageKey": "FONT_SIZE",
        "defaultValue": 24,
        "label": "Font Size",
        "min": 14,
        "max": 28,
        "step": 2
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save Settings"
  }
];
var clay = new Clay(clayConfig, null, {
  autoHandleEvents: false
});

let geminiApiKey = localStorage.getItem("GEMINI_API_KEY") || "";
let ttsApiKey = localStorage.getItem("TTS_API_KEY") || "";
let fontSize = localStorage.getItem("FONT_SIZE") || 24;
let customPersona = localStorage.getItem("CUSTOM_PERSONA") || "";

// Chat History State
let conversationHistory = [];
try {
  let storedHistory = localStorage.getItem("CHAT_HISTORY");
  if (storedHistory) {
    conversationHistory = JSON.parse(storedHistory);
  }
} catch (e) {
  conversationHistory = [];
}

// Dual-Queue Architecture State
let sentenceQueue = [];      // Text waiting for TTS
let readyAudioQueue = [];    // ADPCM bytes waiting for BLE
let isFetchingTTS = false;
let isSendingBLE = false;

let js_adpcm_valpred = 0;
let js_adpcm_index = 0;

const stepTable = [
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
  34,37,41,45,50,55,60,66,73,80,88,97,107,118,
  130,143,157,173,190,209,230,253,279,307,337,
  371,408,449,494,544,598,658,724,796,876,963,
  1060,1166,1282,1411,1552,1707,1878,2066,2272,
  2499,2749,3024,3327,3660,4026,4428,4871,5358,
  5894,6484,7132,7845,8630,9493,10442,11487,
  12635,13899,15289,16818,18500,20350,22385,
  24623,27086,29794,32767
];

const indexTable = [
  -1,-1,-1,-1,2,4,6,8,
  -1,-1,-1,-1,2,4,6,8
];

function encodeADPCM(pcm16) {
  let adpcm = [];
  let buffer = 0;
  let toggle = false;

  for (let i = 0; i < pcm16.length; i++) {
    let diff = pcm16[i] - js_adpcm_valpred;
    let sign = (diff < 0) ? 8 : 0;
    if (sign) diff = -diff;

    let step = stepTable[js_adpcm_index];
    let delta = 0;
    let vpdiff = (step >> 3);

    if (diff >= step) { delta |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { delta |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { delta |= 1; vpdiff += step; }

    if (sign) { js_adpcm_valpred -= vpdiff; } 
    else { js_adpcm_valpred += vpdiff; }

    if (js_adpcm_valpred > 32767) js_adpcm_valpred = 32767;
    if (js_adpcm_valpred < -32768) js_adpcm_valpred = -32768;

    delta |= sign;
    js_adpcm_index += indexTable[delta];

    if (js_adpcm_index < 0) js_adpcm_index = 0;
    if (js_adpcm_index > 88) js_adpcm_index = 88;

    if (toggle) {
      buffer |= (delta & 0x0F);
      adpcm.push(buffer);
      toggle = false;
    } else {
      buffer = (delta & 0x0F) << 4;
      toggle = true;
    }
  }

  if (toggle) { adpcm.push(buffer); }
  return adpcm;
}

function lowPassFilter(samples) {
  let filtered = [];
  filtered[0] = samples[0];
  filtered[1] = samples[1];
  for (let i = 2; i < samples.length; i++) {
    filtered[i] = (samples[i] + samples[i - 1] + samples[i - 2]) / 3;
  }
  return filtered;
}

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (e && !e.response) return;
  var settings = JSON.parse(decodeURIComponent(e.response));

  if (settings.CUSTOM_PERSONA) {
    customPersona = settings.CUSTOM_PERSONA.value;
    localStorage.setItem("CUSTOM_PERSONA", customPersona);
  }

  if (settings.GEMINI_API_KEY) {
    geminiApiKey = settings.GEMINI_API_KEY.value;
    localStorage.setItem("GEMINI_API_KEY", geminiApiKey);
  }

  if (settings.TTS_API_KEY) {
    ttsApiKey = settings.TTS_API_KEY.value;
    localStorage.setItem("TTS_API_KEY", ttsApiKey);
  }

  if (settings.FONT_SIZE) {
    fontSize = settings.FONT_SIZE.value;
    localStorage.setItem("FONT_SIZE", fontSize);
    Pebble.sendAppMessage({ 'COMMAND': 'UPDATE_FONT', 'FONT_SIZE': parseInt(fontSize) });
  }
});

Pebble.addEventListener('ready', function() {
  if (fontSize != 24) {
    Pebble.sendAppMessage({ 'COMMAND': 'UPDATE_FONT', 'FONT_SIZE': parseInt(fontSize) });
  }
});

Pebble.addEventListener('appmessage', function(e) {
  const dict = e.payload;

  if (dict.COMMAND === 'DICTATION') {
    if (!geminiApiKey) geminiApiKey = localStorage.getItem("GEMINI_API_KEY") || "";
    if (!ttsApiKey) ttsApiKey = localStorage.getItem("TTS_API_KEY") || "";

    if (!geminiApiKey || !ttsApiKey) {
      sendErrorToWatch("Missing API key.");
      return;
    }
    fetchGeminiResponse(dict.TEXT);
  }
});

function sendErrorToWatch(msg) {
  Pebble.sendAppMessage({ 'COMMAND': 'TEXT_RESPONSE', 'TEXT': msg });
}

function fetchGeminiResponse(prompt) {
  // Reset queues for new prompt
  sentenceQueue = [];
  readyAudioQueue = [];
  isFetchingTTS = false;
  isSendingBLE = false;
  js_adpcm_valpred = 0;
  js_adpcm_index = 0;

  const localTime = new Date().toLocaleString();
  let systemText = `You are a smartwatch voice assistant. Responses must be conversational and under 100 words. No markdown or bullet lists. Current local time: ${localTime}.`;
  
  if (customPersona) {
    systemText += `\nUser's custom instructions: ${customPersona}`;
  }

  // Truncate history to save memory and tokens (keep last 6 turns)
  if (conversationHistory.length > 6) {
    let excess = conversationHistory.length - 6;
    if (conversationHistory[excess].role === 'model') { excess++; }
    conversationHistory = conversationHistory.slice(excess);
  }

  conversationHistory.push({ role: "user", parts: [{ text: prompt }] });

  const url = `https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=${geminiApiKey}`;

  fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      system_instruction: { parts: [{ text: systemText }] },
      contents: conversationHistory 
    })
  })
  .then(response => response.json())
  .then(data => {
    if (data.candidates && data.candidates.length > 0) {
      const replyText = data.candidates[0].content.parts[0].text;
      
      conversationHistory.push({ role: "model", parts: [{ text: replyText }] });
      localStorage.setItem("CHAT_HISTORY", JSON.stringify(conversationHistory));

      Pebble.sendAppMessage({ 'COMMAND': 'TEXT_RESPONSE', 'TEXT': replyText.substring(0, 2048) });

      let rawSentences = replyText.match(/[^.!?]+[.!?]*/g) || [replyText];
      sentenceQueue = rawSentences.map(s => s.trim()).filter(s => s.length > 0);

      startTTSFetchLoop(); // Start aggressive background downloading

    } else if (data.error && data.error.message) {
      conversationHistory.pop(); 
      sendErrorToWatch("Gemini Err: " + data.error.message.substring(0, 60));
    } else {
      conversationHistory.pop();
      sendErrorToWatch("Empty Gemini response.");
    }
  })
  .catch(() => {
    conversationHistory.pop();
    sendErrorToWatch("Network error.");
  });
}

// LOOP 1: Fetch and encode TTS as fast as possible in the background
function startTTSFetchLoop() {
  if (isFetchingTTS || sentenceQueue.length === 0) return;
  isFetchingTTS = true;

  let sentence = sentenceQueue.shift();
  const url = `https://texttospeech.googleapis.com/v1/text:synthesize?key=${ttsApiKey}`;

  fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      input: { text: sentence },
      voice: { languageCode: 'en-US', name: 'en-US-Journey-F' },
      audioConfig: { audioEncoding: 'LINEAR16', sampleRateHertz: 8000, speakingRate: 0.92 }
    })
  })
  .then(response => response.json())
  .then(data => {
    if (data.audioContent) {
      let decoded = atob(data.audioContent);
      let pcm16 = [];

      for (let i = 44; i < decoded.length; i += 2) {
        let low = decoded.charCodeAt(i);
        let high = decoded.charCodeAt(i + 1);
        let sample16 = (high << 8) | low;
        if (sample16 > 32767) { sample16 -= 65536; }
        pcm16.push(sample16);
      }

      pcm16 = lowPassFilter(pcm16);
      let adpcmBytes = encodeADPCM(pcm16);
      
      // Push encoded audio to the BLE pipeline
      readyAudioQueue.push(adpcmBytes);
    }
    
    isFetchingTTS = false;
    
    // If the BLE pipeline is sleeping (e.g. waiting for the first sentence), wake it up
    if (!isSendingBLE) {
        startBLESendLoop();
    }
    
    // Immediately fetch the next sentence
    startTTSFetchLoop();
  })
  .catch(function(err) {
    console.log('TTS Error', err);
    isFetchingTTS = false;
    startTTSFetchLoop();
  });
}

// LOOP 2: Take ready audio from the queue and send to the watch via BLE
function startBLESendLoop() {
  if (isSendingBLE) return;
  if (readyAudioQueue.length === 0) return; // Wait for the TTS loop to encode more audio
  
  isSendingBLE = true;
  let adpcmBytes = readyAudioQueue.shift();
  let cursor = 0;

  function sendNextChunk() {
    let previousCursor = cursor;
    let chunk = [];
    let chunkSize = 700;

    for (let i = 0; i < chunkSize && cursor < adpcmBytes.length; i++, cursor++) {
      chunk.push(adpcmBytes[cursor]);
    }

    if (chunk.length > 0) {
      Pebble.sendAppMessage(
        { 'COMMAND': 'AUDIO_CHUNK', 'CHUNK': chunk },
        function success() { setTimeout(sendNextChunk, 60); },
        function failure() { cursor = previousCursor; setTimeout(sendNextChunk, 200); }
      );
    } else {
      Pebble.sendAppMessage(
        { 'COMMAND': 'AUDIO_SENTENCE_END' },
        function() { 
            isSendingBLE = false;
            setTimeout(startBLESendLoop, 120); 
        },
        function() { 
            isSendingBLE = false;
            setTimeout(startBLESendLoop, 300); 
        }
      );
    }
  }
  sendNextChunk();
}