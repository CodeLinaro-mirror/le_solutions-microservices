# SimpleInputMessage


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**role** | **str** | The role of the message (e.g. \&quot;system\&quot;, \&quot;assistant\&quot;, \&quot;user\&quot;). | 
**content** | **str** | The content of the message. | 

## Example

```python
from openapi_client.models.simple_input_message import SimpleInputMessage

# TODO update the JSON string below
json = "{}"
# create an instance of SimpleInputMessage from a JSON string
simple_input_message_instance = SimpleInputMessage.from_json(json)
# print the JSON string representation of the object
print(SimpleInputMessage.to_json())

# convert the object into a dict
simple_input_message_dict = simple_input_message_instance.to_dict()
# create an instance of SimpleInputMessage from a dict
simple_input_message_from_dict = SimpleInputMessage.from_dict(simple_input_message_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


