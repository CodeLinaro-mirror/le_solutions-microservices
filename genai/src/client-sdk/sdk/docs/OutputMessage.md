# OutputMessage

An output message from the model. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The unique ID of the output message.  | 
**type** | **str** | The type of the output message. Always &#x60;message&#x60;.  | 
**role** | **str** | The role of the output message. Always &#x60;assistant&#x60;.  | 
**content** | [**List[OutputContent]**](OutputContent.md) | The content of the output message.  | 
**status** | **str** | The status of the message input. One of &#x60;in_progress&#x60;, &#x60;completed&#x60;, or &#x60;incomplete&#x60;. Populated when input items are returned via API.  | 

## Example

```python
from openapi_client.models.output_message import OutputMessage

# TODO update the JSON string below
json = "{}"
# create an instance of OutputMessage from a JSON string
output_message_instance = OutputMessage.from_json(json)
# print the JSON string representation of the object
print(OutputMessage.to_json())

# convert the object into a dict
output_message_dict = output_message_instance.to_dict()
# create an instance of OutputMessage from a dict
output_message_from_dict = OutputMessage.from_dict(output_message_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


