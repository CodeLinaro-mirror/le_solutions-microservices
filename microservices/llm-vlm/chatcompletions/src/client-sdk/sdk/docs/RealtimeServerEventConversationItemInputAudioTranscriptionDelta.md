# RealtimeServerEventConversationItemInputAudioTranscriptionDelta

Returned when the text value of an input audio transcription content part is updated. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | The unique ID of the server event. | 
**type** | **str** | The event type, must be &#x60;conversation.item.input_audio_transcription.delta&#x60;. | 
**item_id** | **str** | The ID of the item. | 
**content_index** | **int** | The index of the content part in the item&#39;s content array. | [optional] 
**delta** | **str** | The text delta. | [optional] 
**logprobs** | [**List[LogProbProperties]**](LogProbProperties.md) | The log probabilities of the transcription. | [optional] 

## Example

```python
from openapi_client.models.realtime_server_event_conversation_item_input_audio_transcription_delta import RealtimeServerEventConversationItemInputAudioTranscriptionDelta

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeServerEventConversationItemInputAudioTranscriptionDelta from a JSON string
realtime_server_event_conversation_item_input_audio_transcription_delta_instance = RealtimeServerEventConversationItemInputAudioTranscriptionDelta.from_json(json)
# print the JSON string representation of the object
print(RealtimeServerEventConversationItemInputAudioTranscriptionDelta.to_json())

# convert the object into a dict
realtime_server_event_conversation_item_input_audio_transcription_delta_dict = realtime_server_event_conversation_item_input_audio_transcription_delta_instance.to_dict()
# create an instance of RealtimeServerEventConversationItemInputAudioTranscriptionDelta from a dict
realtime_server_event_conversation_item_input_audio_transcription_delta_from_dict = RealtimeServerEventConversationItemInputAudioTranscriptionDelta.from_dict(realtime_server_event_conversation_item_input_audio_transcription_delta_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


