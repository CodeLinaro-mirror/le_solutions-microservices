# TemplateInputMessagesTemplateInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**role** | **str** | The role of the message input. One of &#x60;user&#x60;, &#x60;assistant&#x60;, &#x60;system&#x60;, or &#x60;developer&#x60;.  | 
**content** | [**EvalItemContent**](EvalItemContent.md) |  | 
**type** | **str** | The type of the message input. Always &#x60;message&#x60;.  | [optional] 

## Example

```python
from openapi_client.models.template_input_messages_template_inner import TemplateInputMessagesTemplateInner

# TODO update the JSON string below
json = "{}"
# create an instance of TemplateInputMessagesTemplateInner from a JSON string
template_input_messages_template_inner_instance = TemplateInputMessagesTemplateInner.from_json(json)
# print the JSON string representation of the object
print(TemplateInputMessagesTemplateInner.to_json())

# convert the object into a dict
template_input_messages_template_inner_dict = template_input_messages_template_inner_instance.to_dict()
# create an instance of TemplateInputMessagesTemplateInner from a dict
template_input_messages_template_inner_from_dict = TemplateInputMessagesTemplateInner.from_dict(template_input_messages_template_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


