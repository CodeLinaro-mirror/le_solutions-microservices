# TranscriptTextDoneEvent

Emitted when the transcription is complete. Contains the complete transcription text. Only emitted when you [create a transcription](/docs/api-reference/audio/create-transcription) with the `Stream` parameter set to `true`.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;transcript.text.done&#x60;.  | 
**text** | **str** | The text that was transcribed.  | 
**logprobs** | [**List[TranscriptTextDoneEventLogprobsInner]**](TranscriptTextDoneEventLogprobsInner.md) | The log probabilities of the individual tokens in the transcription. Only included if you [create a transcription](/docs/api-reference/audio/create-transcription) with the &#x60;include[]&#x60; parameter set to &#x60;logprobs&#x60;.  | [optional] 

## Example

```python
from openapi_client.models.transcript_text_done_event import TranscriptTextDoneEvent

# TODO update the JSON string below
json = "{}"
# create an instance of TranscriptTextDoneEvent from a JSON string
transcript_text_done_event_instance = TranscriptTextDoneEvent.from_json(json)
# print the JSON string representation of the object
print(TranscriptTextDoneEvent.to_json())

# convert the object into a dict
transcript_text_done_event_dict = transcript_text_done_event_instance.to_dict()
# create an instance of TranscriptTextDoneEvent from a dict
transcript_text_done_event_from_dict = TranscriptTextDoneEvent.from_dict(transcript_text_done_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


