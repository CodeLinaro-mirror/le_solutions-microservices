# RealtimeClientEventConversationItemRetrieve

Send this event when you want to retrieve the server's representation of a specific item in the conversation history. This is useful, for example, to inspect user audio after noise cancellation and VAD. The server will respond with a `conversation.item.retrieved` event,  unless the item does not exist in the conversation history, in which case the  server will respond with an error. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | Optional client-generated ID used to identify this event. | [optional] 
**type** | **str** | The event type, must be &#x60;conversation.item.retrieve&#x60;. | 
**item_id** | **str** | The ID of the item to retrieve. | 

## Example

```python
from openapi_client.models.realtime_client_event_conversation_item_retrieve import RealtimeClientEventConversationItemRetrieve

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeClientEventConversationItemRetrieve from a JSON string
realtime_client_event_conversation_item_retrieve_instance = RealtimeClientEventConversationItemRetrieve.from_json(json)
# print the JSON string representation of the object
print(RealtimeClientEventConversationItemRetrieve.to_json())

# convert the object into a dict
realtime_client_event_conversation_item_retrieve_dict = realtime_client_event_conversation_item_retrieve_instance.to_dict()
# create an instance of RealtimeClientEventConversationItemRetrieve from a dict
realtime_client_event_conversation_item_retrieve_from_dict = RealtimeClientEventConversationItemRetrieve.from_dict(realtime_client_event_conversation_item_retrieve_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


