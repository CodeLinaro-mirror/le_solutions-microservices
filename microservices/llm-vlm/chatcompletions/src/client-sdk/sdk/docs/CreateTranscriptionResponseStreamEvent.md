# CreateTranscriptionResponseStreamEvent


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;transcript.text.delta&#x60;.  | 
**delta** | **str** | The text delta that was additionally transcribed.  | 
**text** | **str** | The text that was transcribed.  | 
**logprobs** | [**List[TranscriptTextDoneEventLogprobsInner]**](TranscriptTextDoneEventLogprobsInner.md) | The log probabilities of the individual tokens in the transcription. Only included if you [create a transcription](/docs/api-reference/audio/create-transcription) with the &#x60;include[]&#x60; parameter set to &#x60;logprobs&#x60;.  | [optional] 

## Example

```python
from openapi_client.models.create_transcription_response_stream_event import CreateTranscriptionResponseStreamEvent

# TODO update the JSON string below
json = "{}"
# create an instance of CreateTranscriptionResponseStreamEvent from a JSON string
create_transcription_response_stream_event_instance = CreateTranscriptionResponseStreamEvent.from_json(json)
# print the JSON string representation of the object
print(CreateTranscriptionResponseStreamEvent.to_json())

# convert the object into a dict
create_transcription_response_stream_event_dict = create_transcription_response_stream_event_instance.to_dict()
# create an instance of CreateTranscriptionResponseStreamEvent from a dict
create_transcription_response_stream_event_from_dict = CreateTranscriptionResponseStreamEvent.from_dict(create_transcription_response_stream_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


