# First Sentences / TikTok Hooks

Short opening lines to rotate through for demos and social clips.

## Reading Assistant

- Hallo! Ich bin bereit. Was lesen wir heute zusammen? Gibt es ein Buch oder einen Text, über den du sprechen möchtest?

## Implementation Notes

The firmware currently injects one hook randomly into the reading-assistant startup prompt in:

```txt
firmware/src/app/AppController.cpp
AppController::maybeSendReadingAssistantIntro()
```

Add new approved hooks both here and to the `READING_ASSISTANT_HOOKS` array there.

Good hooks should be short, clear, and immediately show what the device does: German reading help, vocabulary explanations, translation, pronunciation, or quiz-style practice.
