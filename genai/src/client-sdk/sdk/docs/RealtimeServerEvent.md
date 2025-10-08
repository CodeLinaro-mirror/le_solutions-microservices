# RealtimeServerEvent

A realtime server event. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | The unique ID of the server event. | 
**type** | **str** | The event type, must be &#x60;conversation.created&#x60;. | 
**conversation** | [**RealtimeServerEventConversationCreatedConversation**](RealtimeServerEventConversationCreatedConversation.md) |  | 
**previous_item_id** | **str** | The ID of the preceding item after which the new item will be inserted.  | 
**item** | [**RealtimeConversationItem**](RealtimeConversationItem.md) |  | 
**item_id** | **str** | The ID of the item. | 
**content_index** | **int** | The index of the content part in the item&#39;s content array. | 
**transcript** | **str** | The final transcript of the audio. | 
**logprobs** | [**List[LogProbProperties]**](LogProbProperties.md) | The log probabilities of the transcription. | [optional] 
**delta** | **str** | The text delta. | 
**error** | [**RealtimeServerEventErrorError**](RealtimeServerEventErrorError.md) |  | 
**audio_end_ms** | **int** | Milliseconds since the session started when speech stopped. This will  correspond to the end of audio sent to the model, and thus includes the  &#x60;min_silence_duration_ms&#x60; configured in the Session.  | 
**audio_start_ms** | **int** | Milliseconds from the start of all audio written to the buffer during the  session when speech was first detected. This will correspond to the  beginning of audio sent to the model, and thus includes the  &#x60;prefix_padding_ms&#x60; configured in the Session.  | 
**rate_limits** | [**List[RealtimeServerEventRateLimitsUpdatedRateLimitsInner]**](RealtimeServerEventRateLimitsUpdatedRateLimitsInner.md) | List of rate limit information. | 
**response_id** | **str** | The unique ID of the response that produced the audio. | 
**output_index** | **int** | The index of the output item in the response. | 
**part** | [**RealtimeServerEventResponseContentPartDonePart**](RealtimeServerEventResponseContentPartDonePart.md) |  | 
**response** | [**RealtimeResponse**](RealtimeResponse.md) |  | 
**call_id** | **str** | The ID of the function call. | 
**arguments** | **str** | The final arguments as a JSON string. | 
**text** | **str** | The final text content. | 
**session** | [**RealtimeTranscriptionSessionCreateResponse**](RealtimeTranscriptionSessionCreateResponse.md) |  | 

## Example

```python
from openapi_client.models.realtime_server_event import RealtimeServerEvent

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeServerEvent from a JSON string
realtime_server_event_instance = RealtimeServerEvent.from_json(json)
# print the JSON string representation of the object
print(RealtimeServerEvent.to_json())

# convert the object into a dict
realtime_server_event_dict = realtime_server_event_instance.to_dict()
# create an instance of RealtimeServerEvent from a dict
realtime_server_event_from_dict = RealtimeServerEvent.from_dict(realtime_server_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


