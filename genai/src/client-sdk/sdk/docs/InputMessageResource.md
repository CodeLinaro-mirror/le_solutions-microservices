# InputMessageResource


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the message input. Always set to &#x60;message&#x60;.  | [optional] 
**role** | **str** | The role of the message input. One of &#x60;user&#x60;, &#x60;system&#x60;, or &#x60;developer&#x60;.  | 
**status** | **str** | The status of item. One of &#x60;in_progress&#x60;, &#x60;completed&#x60;, or &#x60;incomplete&#x60;. Populated when items are returned via API.  | [optional] 
**content** | [**List[InputContent]**](InputContent.md) | A list of one or many input items to the model, containing different content  types.  | 
**id** | **str** | The unique ID of the message input.  | 

## Example

```python
from openapi_client.models.input_message_resource import InputMessageResource

# TODO update the JSON string below
json = "{}"
# create an instance of InputMessageResource from a JSON string
input_message_resource_instance = InputMessageResource.from_json(json)
# print the JSON string representation of the object
print(InputMessageResource.to_json())

# convert the object into a dict
input_message_resource_dict = input_message_resource_instance.to_dict()
# create an instance of InputMessageResource from a dict
input_message_resource_from_dict = InputMessageResource.from_dict(input_message_resource_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


