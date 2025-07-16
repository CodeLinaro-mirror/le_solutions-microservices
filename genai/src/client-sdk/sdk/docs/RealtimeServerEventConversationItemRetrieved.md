# RealtimeServerEventConversationItemRetrieved

Returned when a conversation item is retrieved with `conversation.item.retrieve`. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**event_id** | **str** | The unique ID of the server event. | 
**type** | **str** | The event type, must be &#x60;conversation.item.retrieved&#x60;. | 
**item** | [**RealtimeConversationItem**](RealtimeConversationItem.md) |  | 

## Example

```python
from openapi_client.models.realtime_server_event_conversation_item_retrieved import RealtimeServerEventConversationItemRetrieved

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeServerEventConversationItemRetrieved from a JSON string
realtime_server_event_conversation_item_retrieved_instance = RealtimeServerEventConversationItemRetrieved.from_json(json)
# print the JSON string representation of the object
print(RealtimeServerEventConversationItemRetrieved.to_json())

# convert the object into a dict
realtime_server_event_conversation_item_retrieved_dict = realtime_server_event_conversation_item_retrieved_instance.to_dict()
# create an instance of RealtimeServerEventConversationItemRetrieved from a dict
realtime_server_event_conversation_item_retrieved_from_dict = RealtimeServerEventConversationItemRetrieved.from_dict(realtime_server_event_conversation_item_retrieved_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


