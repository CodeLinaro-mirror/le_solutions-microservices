# TemplateInputMessages


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of input messages. Always &#x60;template&#x60;. | 
**template** | [**List[TemplateInputMessagesTemplateInner]**](TemplateInputMessagesTemplateInner.md) | A list of chat messages forming the prompt or context. May include variable references to the \&quot;item\&quot; namespace, ie {{item.name}}. | 

## Example

```python
from openapi_client.models.template_input_messages import TemplateInputMessages

# TODO update the JSON string below
json = "{}"
# create an instance of TemplateInputMessages from a JSON string
template_input_messages_instance = TemplateInputMessages.from_json(json)
# print the JSON string representation of the object
print(TemplateInputMessages.to_json())

# convert the object into a dict
template_input_messages_dict = template_input_messages_instance.to_dict()
# create an instance of TemplateInputMessages from a dict
template_input_messages_from_dict = TemplateInputMessages.from_dict(template_input_messages_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


