# RealtimeClientEvent

A realtime client event. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | Optional client-generated ID used to identify this event. | [optional] 
**type** | **str** | The event type, must be &#x60;conversation.item.create&#x60;. | 
**previous_item_id** | **str** | The ID of the preceding item after which the new item will be inserted.  If not set, the new item will be appended to the end of the conversation. If set to &#x60;root&#x60;, the new item will be added to the beginning of the conversation. If set to an existing ID, it allows an item to be inserted mid-conversation. If the ID cannot be found, an error will be returned and the item will not be added.  | [optional] 
**item** | [**RealtimeConversationItem**](RealtimeConversationItem.md) |  | 
**item_id** | **str** | The ID of the assistant message item to truncate. Only assistant message  items can be truncated.  | 
**content_index** | **int** | The index of the content part to truncate. Set this to 0. | 
**audio_end_ms** | **int** | Inclusive duration up to which audio is truncated, in milliseconds. If  the audio_end_ms is greater than the actual audio duration, the server  will respond with an error.  | 
**audio** | **str** | Base64-encoded audio bytes. This must be in the format specified by the  &#x60;input_audio_format&#x60; field in the session configuration.  | 
**response_id** | **str** | A specific response ID to cancel - if not provided, will cancel an  in-progress response in the default conversation.  | [optional] 
**response** | [**RealtimeResponseCreateParams**](RealtimeResponseCreateParams.md) |  | [optional] 
**session** | [**RealtimeTranscriptionSessionCreateRequest**](RealtimeTranscriptionSessionCreateRequest.md) |  | 

## Example

```python
from openapi_client.models.realtime_client_event import RealtimeClientEvent

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeClientEvent from a JSON string
realtime_client_event_instance = RealtimeClientEvent.from_json(json)
# print the JSON string representation of the object
print(RealtimeClientEvent.to_json())

# convert the object into a dict
realtime_client_event_dict = realtime_client_event_instance.to_dict()
# create an instance of RealtimeClientEvent from a dict
realtime_client_event_from_dict = RealtimeClientEvent.from_dict(realtime_client_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


